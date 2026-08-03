#pragma once
// VocalTrack.h — single-channel vocal recording track.
//
// STATE MACHINE (message thread drives transitions via atomics):
//
//   Idle ──arm()──► Armed ──startMonitoring()──► Monitoring
//     ▲               │                              │
//     │          startRecording()            startRecording()
//     │               │                              │
//     │               └────────────┬─────────────────┘
//     │                            ▼
//     │                        Recording
//     │                            │
//     │                      stopRecording()
//     │                            │
//     │                            ▼
//     └────── (worker drains ring-buffer then sets Idle) ── Stopping
//
// AUDIO THREAD (processBlock):
//   - Reads state_ via atomic acquire — no lock, no allocation.
//   - When Recording: writes input ch 0 → RecordBuffer (SPSC, lock-free).
//   - When Monitoring or Recording: adds input ch 0 → output ch 0 (dry monitor).
//   - Never touches WavWriter, TakeManager, or any heap allocation.
//
// WORKER THREAD (drainToFile):
//   - Reads RecordBuffer → writes to WavWriter.
//   - Holds wavWriterMutex_ (std::mutex) during I/O.
//   - When state == Stopping and buffer is empty: closes file, → Idle.
//
// MESSAGE THREAD:
//   - arm/disarm/startMonitoring/stopMonitoring/startRecording/stopRecording.
//   - openNewTake() / setTakeDirectory().

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>
#include "RecordBuffer.h"
#include "TakeManager.h"
#include "WavWriter.h"
#include "PlaybackBuffer.h"
#include "SessionDocument.h"
#include "WaveformCache.h"

namespace ssbb {

class VocalTrack
{
public:
    enum class State : int
    {
        Idle       = 0,
        Armed      = 1,
        Monitoring = 2,
        Recording  = 3,
        Stopping   = 4
    };

    enum class WorkerStatus : int
    {
        Idle = 0,
        ImportPending,
        ImportSucceeded,
        ImportFailed,
        ExportPending,
        ExportSucceeded,
        ExportFailed,
        AutosaveSucceeded,
        AutosaveFailed,
        RecoverySucceeded,
        RecoveryFailed
    };

    VocalTrack();
    ~VocalTrack() = default;

    // Non-copyable / non-movable (contains atomics + mutex).
    VocalTrack(const VocalTrack&)            = delete;
    VocalTrack& operator=(const VocalTrack&) = delete;
    VocalTrack(VocalTrack&&)                 = delete;
    VocalTrack& operator=(VocalTrack&&)      = delete;

    // ---- Prepare (called from device setup thread before audio starts) ---

    /// Cache sample rate / block size for use by openNewTake().
    void prepare(double sampleRate, int blockSize) noexcept;

    // ---- Message-thread state-machine API --------------------------------

    /// Idle → Armed.
    void arm() noexcept;

    /// Armed → Idle  OR  Monitoring → Idle.
    void disarm() noexcept;

    /// Armed → Monitoring (enable live input monitoring without recording).
    void startMonitoring() noexcept;

    /// Monitoring → Armed.
    void stopMonitoring() noexcept;

    /// Armed or Monitoring → Recording.
    /// Opens a new timestamped take file via TakeManager.
    void startRecording();

    /// Recording → Stopping.
    /// The worker thread will drain the remaining ring-buffer samples to disk
    /// and then transition to Idle automatically.
    void stopRecording() noexcept;

    /// Read the current state from any thread (atomic acquire).
    [[nodiscard]]
    State getState() const noexcept
    {
        return static_cast<State>(state_.load(std::memory_order_acquire));
    }

    // ---- Take management (message thread) --------------------------------

    /// Set the directory where new take files are written.
    /// Creates the directory if it does not exist.
    void setTakeDirectory(const std::filesystem::path& dir);

    /// Create a new take file for the next recording.
    /// Called automatically by startRecording(); expose for testing.
    bool openNewTake();

    [[nodiscard]] bool hasPlayback() const noexcept { return playback_.isReady(); }
    [[nodiscard]] int64_t getPlaybackLengthSamples() const noexcept
    {
        return playback_.numFrames();
    }
    [[nodiscard]] double getPlaybackSampleRate() const noexcept
    {
        return playback_.sampleRate();
    }
    [[nodiscard]] bool hasRecordingError() const noexcept
    {
        return recordingError_.load(std::memory_order_acquire);
    }

    void requestImport(const std::filesystem::path& wavPath);
    void requestExport(const std::filesystem::path& wavPath);
    void requestAutosave() noexcept;
    void requestRecoveryLoad() noexcept;

    [[nodiscard]] WorkerStatus getWorkerStatus() const noexcept
    {
        return static_cast<WorkerStatus>(workerStatus_.load(std::memory_order_acquire));
    }

    [[nodiscard]] bool recoveryAvailable() const;

    /// Copy a new immutable waveform snapshot for the UI. Returns true only
    /// when `generation` was stale and `out` was updated.
    bool copyWaveformIfChanged(std::vector<WaveformCache::Frame>& out,
                               uint64_t& generation) const;

    void setClipOffsetSamples(int64_t value);
    void setTrimStartSamples(int64_t value);
    void setTrimEndSamples(int64_t value);
    [[nodiscard]] int64_t getClipOffsetSamples() const noexcept
    { return clipOffsetSamples_.load(std::memory_order_relaxed); }
    [[nodiscard]] int64_t getTrimStartSamples() const noexcept
    { return trimStartSamples_.load(std::memory_order_relaxed); }
    [[nodiscard]] int64_t getTrimEndSamples() const noexcept
    { return trimEndSamples_.load(std::memory_order_relaxed); }

    /// Access the underlying TakeManager (for session persistence).
    TakeManager& getTakeManager() noexcept { return takeManager_; }

    // ---- AUDIO THREAD -------------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Mix input monitoring and capture to ring buffer.
    ///
    /// @param inputChannelData   Array of input channel pointers (may be null).
    /// @param numInputChannels   Length of inputChannelData array.
    /// @param outputChannelData  Array of output channel pointers (may be null).
    /// @param numOutputChannels  Length of outputChannelData array.
    /// @param numSamples         Block size in samples.
    void processBlock(const float* const* inputChannelData,
                      int                 numInputChannels,
                      float* const*       outputChannelData,
                      int                 numOutputChannels,
                      int                 numSamples,
                      int64_t             blockStartSamples,
                      bool                transportPlaying) noexcept;

    // ---- WORKER THREAD -------------------------------------------------------

    /// Drain the ring buffer to the current WAV file.
    /// When state == Stopping and the buffer is empty, closes the file and
    /// transitions state to Idle.
    void drainToFile();

    /// Drain recording data and service import/export/autosave/recovery jobs.
    /// Called only by AudioEngine's worker thread.
    void serviceWorkerTasks();

private:
    // ---- Audio-thread-safe members (atomics only) -----------------------

    /// Current recording state.  Written by message/worker thread,
    /// read by audio thread with acquire ordering.
    std::atomic<int> state_ { static_cast<int>(State::Idle) };

    /// Lock-free SPSC ring buffer.
    /// Audio thread writes (producer), worker thread reads (consumer).
    /// Capacity = 1<<18 = 262 144 samples ≈ 5.9 s at 44.1 kHz.
    RecordBuffer<(1u << 18u)> recordBuffer_;

    /// Sample rate set by prepare() and used when creating take files.
    std::atomic<double> sampleRate_ { 44100.0 };
    std::atomic<bool> recordingError_ { false };
    std::atomic<int> workerStatus_ { static_cast<int>(WorkerStatus::Idle) };
    std::atomic<bool> autosaveRequested_ { false };
    std::atomic<bool> recoveryRequested_ { false };
    std::atomic<bool> recoveryAvailable_ { false };
    std::atomic<int64_t> clipOffsetSamples_ { 0 };
    std::atomic<int64_t> trimStartSamples_ { 0 };
    std::atomic<int64_t> trimEndSamples_ { 0 };

    // ---- Worker / message thread shared (protected by wavWriterMutex_) --
    //
    // The audio thread NEVER touches wavWriter_ or wavWriterMutex_.

    std::mutex wavWriterMutex_;
    WavWriter  wavWriter_;
    std::filesystem::path currentTakePath_;
    PlaybackBuffer playback_;
    WaveformCache waveformCache_;

    mutable std::mutex jobMutex_;
    std::filesystem::path pendingImportPath_;
    std::filesystem::path pendingExportPath_;

    mutable std::mutex waveformMutex_;
    std::vector<WaveformCache::Frame> waveformFrames_;
    uint64_t waveformGeneration_ { 0 };

    mutable std::mutex sessionMutex_;
    SessionData sessionData_;
    std::filesystem::path sessionPath_;

    // ---- Message-thread-only members ------------------------------------

    std::filesystem::path takeDir_;
    TakeManager           takeManager_;

    void publishWaveform(const std::filesystem::path& path);
    void appendSourceToSession(const std::filesystem::path& path,
                               double sampleRate,
                               int channels,
                               int64_t frames,
                               const char* sourceLabel);
    void updateSessionClipEdits();
};

} // namespace ssbb
