// VocalTrack.cpp
//
// Audio-thread rules apply ONLY to processBlock():
//   NO allocation, NO locks, NO logging, NO file I/O, NO exceptions.
//
// All other methods run on the message thread or worker thread and may use
// std::mutex, std::filesystem, and other non-RT-safe facilities freely.
#include "VocalTrack.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace ssbb {

// ---- Constructor ----------------------------------------------------------

VocalTrack::VocalTrack()
{
    // Provide a safe default take directory (overridden by the application).
    takeDir_ = std::filesystem::temp_directory_path() / "ssbb_takes";
    sessionPath_ = takeDir_ / "session.json";
    sessionData_.version = SessionDocument::kSchemaVersion;
}

// ---- Prepare (device setup thread) ---------------------------------------

void VocalTrack::prepare(double sampleRate, int /*blockSize*/) noexcept
{
    sampleRate_.store((sampleRate > 0.0) ? sampleRate : 44100.0,
                      std::memory_order_relaxed);
}

// ---- Message-thread state-machine ----------------------------------------

void VocalTrack::arm() noexcept
{
    int expected = static_cast<int>(State::Idle);
    state_.compare_exchange_strong(expected,
                                   static_cast<int>(State::Armed),
                                   std::memory_order_acq_rel,
                                   std::memory_order_relaxed);
}

void VocalTrack::disarm() noexcept
{
    // Armed → Idle
    int expected = static_cast<int>(State::Armed);
    if (state_.compare_exchange_strong(expected,
                                       static_cast<int>(State::Idle),
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed))
        return;

    // Monitoring → Idle
    expected = static_cast<int>(State::Monitoring);
    state_.compare_exchange_strong(expected,
                                   static_cast<int>(State::Idle),
                                   std::memory_order_acq_rel,
                                   std::memory_order_relaxed);
}

void VocalTrack::startMonitoring() noexcept
{
    int expected = static_cast<int>(State::Armed);
    state_.compare_exchange_strong(expected,
                                   static_cast<int>(State::Monitoring),
                                   std::memory_order_acq_rel,
                                   std::memory_order_relaxed);
}

void VocalTrack::stopMonitoring() noexcept
{
    int expected = static_cast<int>(State::Monitoring);
    state_.compare_exchange_strong(expected,
                                   static_cast<int>(State::Armed),
                                   std::memory_order_acq_rel,
                                   std::memory_order_relaxed);
}

void VocalTrack::setTakeDirectory(const std::filesystem::path& dir)
{
    takeDir_ = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::lock_guard<std::mutex> lock(jobMutex_);
    sessionPath_ = dir / "session.json";
    const auto recovery = dir / "session.recovery.json";
    recoveryAvailable_.store(std::filesystem::is_regular_file(recovery, ec),
                             std::memory_order_release);
    // (ec is intentionally ignored — failure will surface when opening the file)
}

void VocalTrack::requestImport(const std::filesystem::path& wavPath)
{
    if (wavPath.empty()) return;
    std::lock_guard<std::mutex> lock(jobMutex_);
    pendingImportPath_ = wavPath;
    workerStatus_.store(static_cast<int>(WorkerStatus::ImportPending),
                        std::memory_order_release);
}

void VocalTrack::requestExport(const std::filesystem::path& wavPath)
{
    if (wavPath.empty()) return;
    std::lock_guard<std::mutex> lock(jobMutex_);
    pendingExportPath_ = wavPath;
    workerStatus_.store(static_cast<int>(WorkerStatus::ExportPending),
                        std::memory_order_release);
}

void VocalTrack::requestAutosave() noexcept
{
    autosaveRequested_.store(true, std::memory_order_release);
}

void VocalTrack::requestRecoveryLoad() noexcept
{
    recoveryRequested_.store(true, std::memory_order_release);
}

bool VocalTrack::recoveryAvailable() const
{
    return recoveryAvailable_.load(std::memory_order_acquire);
}

bool VocalTrack::copyWaveformIfChanged(
    std::vector<WaveformCache::Frame>& out,
    uint64_t& generation) const
{
    std::lock_guard<std::mutex> lock(waveformMutex_);
    if (generation == waveformGeneration_)
        return false;
    out = waveformFrames_;
    generation = waveformGeneration_;
    return true;
}

void VocalTrack::setClipOffsetSamples(int64_t value)
{
    clipOffsetSamples_.store(value > 0 ? value : 0, std::memory_order_relaxed);
    updateSessionClipEdits();
    requestAutosave();
}

void VocalTrack::setTrimStartSamples(int64_t value)
{
    const int64_t length = playback_.numFrames();
    const int64_t clamped = value < 0 ? 0 : (value > length ? length : value);
    trimStartSamples_.store(clamped, std::memory_order_relaxed);
    updateSessionClipEdits();
    requestAutosave();
}

void VocalTrack::setTrimEndSamples(int64_t value)
{
    const int64_t length = playback_.numFrames();
    const int64_t clamped = value < 0 ? 0 : (value > length ? length : value);
    trimEndSamples_.store(clamped, std::memory_order_relaxed);
    updateSessionClipEdits();
    requestAutosave();
}

bool VocalTrack::openNewTake()
{
    const double sr = sampleRate_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(wavWriterMutex_);

    // Close any take that was still open (e.g. rapid re-record scenario).
    if (wavWriter_.isOpen())
        wavWriter_.close();

    const auto path = takeManager_.createTakePath(takeDir_, sr, /*numChannels=*/1);
    if (path.empty() || !wavWriter_.open(path, sr, /*numChannels=*/1))
    {
        if (!path.empty())
            takeManager_.discardLastReservation(path);
        currentTakePath_.clear();
        recordingError_.store(true, std::memory_order_release);
        return false;
    }

    currentTakePath_ = path;
    recordingError_.store(false, std::memory_order_release);
    return true;
}

void VocalTrack::startRecording()
{
    // Only allowed from Armed or Monitoring states.
    const int cur = state_.load(std::memory_order_acquire);
    if (cur != static_cast<int>(State::Armed) &&
        cur != static_cast<int>(State::Monitoring))
        return;

    if (!openNewTake())
        return;

    // Transition to Recording: the audio thread will start writing to the
    // ring buffer once it observes this store.
    state_.store(static_cast<int>(State::Recording),
                 std::memory_order_release);
}

void VocalTrack::stopRecording() noexcept
{
    // Recording → Stopping.
    // The audio thread will stop writing immediately; the worker thread will
    // drain the remaining ring-buffer samples and then close the file.
    int expected = static_cast<int>(State::Recording);
    state_.compare_exchange_strong(expected,
                                   static_cast<int>(State::Stopping),
                                   std::memory_order_acq_rel,
                                   std::memory_order_relaxed);
}

// ---- AUDIO THREAD ---------------------------------------------------------
//
// INVARIANTS:
//   - No allocation.       RecordBuffer is a value member; no heap usage here.
//   - No locks.            state_ is atomic; wavWriterMutex_ is NEVER touched.
//   - No file/network I/O. Only RecordBuffer::write() and arithmetic.
//   - No exceptions.       All operations are noexcept.
//   - No logging.          Drops silently if the ring buffer is full.

void VocalTrack::processBlock(const float* const* inputChannelData,
                               int                 numInputChannels,
                               float* const*       outputChannelData,
                               int                 numOutputChannels,
                               int                 numSamples,
                               int64_t             blockStartSamples,
                               bool                transportPlaying) noexcept
{
    if (numSamples <= 0) return;

    const auto state = static_cast<State>(state_.load(std::memory_order_acquire));

    const bool monitor   = (state == State::Monitoring || state == State::Recording);
    const bool capturing = (state == State::Recording);

    const bool hasInput  = (numInputChannels  > 0
                            && inputChannelData  != nullptr
                            && inputChannelData[0]  != nullptr);
    const bool hasOutput = (numOutputChannels > 0
                            && outputChannelData != nullptr
                            && outputChannelData[0] != nullptr);

    // 1. Input monitor: mix (add) dry input ch 0 → output ch 0.
    //    This lets the singer hear themselves during Monitoring and Recording.
    if (monitor && hasInput && hasOutput)
    {
        const float* in  = inputChannelData[0];
        float*       out = outputChannelData[0];
        for (int i = 0; i < numSamples; ++i)
            out[i] += in[i];
    }

    // 2. Capture: push input ch 0 into the lock-free ring buffer.
    //    RecordBuffer::write() drops the block silently if the buffer is full.
    if (capturing && hasInput)
    {
        (void)recordBuffer_.write(inputChannelData[0], numSamples);
    }

    if (transportPlaying && !capturing)
    {
        playback_.renderClip(
            outputChannelData, numOutputChannels, numSamples,
            blockStartSamples,
            sampleRate_.load(std::memory_order_relaxed),
            clipOffsetSamples_.load(std::memory_order_relaxed),
            trimStartSamples_.load(std::memory_order_relaxed),
            trimEndSamples_.load(std::memory_order_relaxed));
    }
}

// ---- WORKER THREAD --------------------------------------------------------

void VocalTrack::drainToFile()
{
    const auto state = static_cast<State>(state_.load(std::memory_order_acquire));

    // Only drain when audio is being (or has been) captured.
    if (state != State::Recording && state != State::Stopping)
        return;

    std::filesystem::path completedTake;
    {
        std::lock_guard<std::mutex> lock(wavWriterMutex_);

        if (!wavWriter_.isOpen())
            return;

    // Drain the ring buffer in chunks.  The temporary array is on the
    // worker thread's stack — no heap allocation.
    constexpr int kChunk = 4096;
    float tmp[kChunk];

        while (true)
        {
            const int n = recordBuffer_.read(tmp, kChunk);
            if (n == 0) break;
            if (!wavWriter_.write(tmp, n))
            {
                recordingError_.store(true, std::memory_order_release);
                state_.store(static_cast<int>(State::Stopping),
                             std::memory_order_release);
                break;
            }
        }

    // If stop was requested and the ring buffer is now completely empty,
    // finalise the take file and transition back to Idle.
        if (state == State::Stopping && recordBuffer_.availableRead() == 0)
        {
            wavWriter_.close();
            completedTake = currentTakePath_;
            currentTakePath_.clear();
            state_.store(static_cast<int>(State::Idle),
                         std::memory_order_release);
        }
    }

    if (!completedTake.empty())
    {
        if (playback_.load(completedTake))
        {
            publishWaveform(completedTake);
            appendSourceToSession(completedTake,
                                  playback_.sampleRate(),
                                  playback_.numChannels(),
                                  playback_.numFrames(),
                                  "recorded");
            requestAutosave();
        }
        else
        {
            recordingError_.store(true, std::memory_order_release);
        }
    }
}

void VocalTrack::publishWaveform(const std::filesystem::path& path)
{
    waveformCache_.buildFromFile(path, 256);
    std::vector<WaveformCache::Frame> next;
    if (waveformCache_.isReady())
        next = waveformCache_.getFrames();

    std::lock_guard<std::mutex> lock(waveformMutex_);
    waveformFrames_ = std::move(next);
    ++waveformGeneration_;
}

void VocalTrack::appendSourceToSession(const std::filesystem::path& path,
                                       double sampleRate,
                                       int channels,
                                       int64_t frames,
                                       const char* sourceLabel)
{
    clipOffsetSamples_.store(0, std::memory_order_relaxed);
    trimStartSamples_.store(0, std::memory_order_relaxed);
    trimEndSamples_.store(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(sessionMutex_);

    TakeEntry take;
    take.path = path.string();
    take.sampleRate = sampleRate;
    take.numChannels = channels;
    take.isoTimestamp = sourceLabel ? sourceLabel : "source";
    sessionData_.takes.push_back(std::move(take));

    ClipEntry clip;
    clip.takePath = path.string();
    clip.sourceLengthSamples = frames;
    sessionData_.clips.push_back(std::move(clip));
    sessionData_.dirty = true;
}

void VocalTrack::updateSessionClipEdits()
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (sessionData_.clips.empty()) return;
    auto& clip = sessionData_.clips.back();
    clip.offsetSamples = clipOffsetSamples_.load(std::memory_order_relaxed);
    clip.trimStartSamples = trimStartSamples_.load(std::memory_order_relaxed);
    clip.trimEndSamples = trimEndSamples_.load(std::memory_order_relaxed);
    sessionData_.dirty = true;
}

void VocalTrack::serviceWorkerTasks()
{
    drainToFile();
    bool servicedForegroundJob = false;

    std::filesystem::path importPath;
    std::filesystem::path exportPath;
    std::filesystem::path sessionPath;
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        importPath.swap(pendingImportPath_);
        exportPath.swap(pendingExportPath_);
        sessionPath = sessionPath_;
    }

    if (!importPath.empty())
    {
        servicedForegroundJob = true;
        if (playback_.load(importPath))
        {
            publishWaveform(importPath);
            appendSourceToSession(importPath,
                                  playback_.sampleRate(),
                                  playback_.numChannels(),
                                  playback_.numFrames(),
                                  "imported");
            workerStatus_.store(static_cast<int>(WorkerStatus::ImportSucceeded),
                                std::memory_order_release);
            requestAutosave();
        }
        else
        {
            workerStatus_.store(static_cast<int>(WorkerStatus::ImportFailed),
                                std::memory_order_release);
        }
    }

    if (!exportPath.empty())
    {
        servicedForegroundJob = true;
        const bool ok = playback_.exportTo(exportPath);
        workerStatus_.store(static_cast<int>(ok ? WorkerStatus::ExportSucceeded
                                                : WorkerStatus::ExportFailed),
                            std::memory_order_release);
    }

    if (recoveryRequested_.exchange(false, std::memory_order_acq_rel))
    {
        servicedForegroundJob = true;
        SessionData recovered;
        bool ok = SessionDocument::loadRecovery(sessionPath, recovered);
        std::filesystem::path source;
        if (ok && !recovered.clips.empty())
            source = recovered.clips.back().takePath;
        else if (ok && !recovered.takes.empty())
            source = recovered.takes.back().path;
        else
            ok = false;

        if (ok)
            ok = playback_.load(source);

        if (ok)
        {
            publishWaveform(source);
            std::lock_guard<std::mutex> lock(sessionMutex_);
            sessionData_ = std::move(recovered);
            if (!sessionData_.clips.empty())
            {
                const auto& clip = sessionData_.clips.back();
                clipOffsetSamples_.store(clip.offsetSamples, std::memory_order_relaxed);
                trimStartSamples_.store(clip.trimStartSamples, std::memory_order_relaxed);
                trimEndSamples_.store(clip.trimEndSamples, std::memory_order_relaxed);
            }
        }

        workerStatus_.store(static_cast<int>(ok ? WorkerStatus::RecoverySucceeded
                                                : WorkerStatus::RecoveryFailed),
                            std::memory_order_release);
    }

    if (!servicedForegroundJob &&
        autosaveRequested_.exchange(false, std::memory_order_acq_rel))
    {
        SessionData snapshot;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            snapshot = sessionData_;
            snapshot.version = SessionDocument::kSchemaVersion;
            snapshot.dirty = true;
        }
        const bool ok = SessionDocument::saveRecovery(sessionPath, snapshot);
        if (ok)
            recoveryAvailable_.store(true, std::memory_order_release);
        workerStatus_.store(static_cast<int>(ok ? WorkerStatus::AutosaveSucceeded
                                                : WorkerStatus::AutosaveFailed),
                            std::memory_order_release);
    }
}

} // namespace ssbb
