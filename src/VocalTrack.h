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
#include "RecordBuffer.h"
#include "TakeManager.h"
#include "WavWriter.h"

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
    void openNewTake();

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
                      int                 numSamples) noexcept;

    // ---- WORKER THREAD -------------------------------------------------------

    /// Drain the ring buffer to the current WAV file.
    /// When state == Stopping and the buffer is empty, closes the file and
    /// transitions state to Idle.
    void drainToFile();

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

    // ---- Worker / message thread shared (protected by wavWriterMutex_) --
    //
    // The audio thread NEVER touches wavWriter_ or wavWriterMutex_.

    std::mutex wavWriterMutex_;
    WavWriter  wavWriter_;

    // ---- Message-thread-only members ------------------------------------

    std::filesystem::path takeDir_;
    TakeManager           takeManager_;
};

} // namespace ssbb
