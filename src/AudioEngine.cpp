// AudioEngine.cpp
// Message-thread methods (constructor, audioDeviceAboutToStart,
// audioDeviceError, audioDeviceStopped) may use JUCE APIs freely.
//
// audioDeviceIOCallbackWithContext is the AUDIO THREAD:
//   NO allocation, NO locks, NO logging, NO JUCE String construction,
//   NO file/network I/O, NO exceptions, NO AI work.
#include "AudioEngine.h"

#include <chrono>
#include <thread>
#include "WavReader.h"

namespace ssbb {

AudioEngine::AudioEngine()
{
    // initialise(maxInputChannels, maxOutputChannels, savedState, selectDefaultDevice)
    // Using explicit form rather than initialiseWithDefaultDevices so callers
    // can later supply a saved XmlElement for recall.
    deviceManager_.initialise(2, 2, nullptr, true);
    deviceManager_.addAudioCallback(this);

    // Start the drain worker thread.  It sleeps 5 ms between drain passes
    // so the maximum latency from record-stop to file-close is ~5 ms.
    workerThread_ = std::thread([this] { workerThreadLoop(); });
}

AudioEngine::~AudioEngine()
{
    // 1. Stop audio callbacks first so the audio thread no longer writes to
    //    vocalTrack_.recordBuffer_.
    deviceManager_.removeAudioCallback(this);

    // 2. Signal the worker thread and wait for it to finish.
    workerStop_.store(true, std::memory_order_release);
    if (workerThread_.joinable())
        workerThread_.join();
    // All members are now safe to destroy.
}

void AudioEngine::workerThreadLoop()
{
    while (!workerStop_.load(std::memory_order_relaxed))
    {
        vocalTrack_.drainToFile();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Final drain: flush any samples captured between the last loop iteration
    // and the audio callback being removed.
    vocalTrack_.drainToFile();
}

// ---- AudioIODeviceCallback (message thread / device thread) ----

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Flush denormals for this thread (the audio device thread).
    // This covers the callback path on platforms that honour MXCSR.
    juce::ScopedNoDenormals noDenormals;

    const double sr        = device->getCurrentSampleRate();
    const int    blockSize = device->getCurrentBufferSizeSamples();

    transport_.prepare(sr, blockSize);
    metronome_.prepare(sr, blockSize);
    vocalTrack_.prepare(sr, blockSize);
    clipPlayer_.prepare(sr, blockSize);

    numInputChannels_.store(
        device->getActiveInputChannels().countNumberOfSetBits(),
        std::memory_order_relaxed);
    numOutputChannels_.store(
        device->getActiveOutputChannels().countNumberOfSetBits(),
        std::memory_order_relaxed);

    const double latencyMs =
        (static_cast<double>(device->getInputLatencyInSamples()) +
         static_cast<double>(device->getOutputLatencyInSamples())) /
        sr * 1000.0;
    estimatedLatencyMs_.store(latencyMs, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceStopped()
{
    transport_.stop();
}

void AudioEngine::audioDeviceError(const juce::String& /*errorMessage*/)
{
    // This may be called from a device thread. Keep it realtime-safe and let
    // the UI observe the stopped transport without logging or allocating here.
    transport_.stop();
}

// ---- AUDIO THREAD ----

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int                 numInputChannels,
    float* const*       outputChannelData,
    int                 numOutputChannels,
    int                 numSamples,
    const juce::AudioIODeviceCallbackContext& /*context*/)
{
    // Flush denormals to zero for this audio callback invocation.
    // ScopedNoDenormals is a stack-only RAII object: no allocation, no lock.
    juce::ScopedNoDenormals noDenormals;

    // ---- AUDIO THREAD: no allocation, no locks, no logging, no exceptions ----

    // 1. Clear all output buffers.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    // 2. Capture the block-start position *before* advancing the transport.
    //    Metronome needs this to correctly place clicks across loop boundaries.
    const int64_t blockStart = transport_.getPositionInSamples();

    // 3. Advance the transport position (atomic store, no allocation).
    transport_.process(numSamples);

    // 4. Mix metronome clicks into channel 0.
    if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
        metronome_.processBlock(outputChannelData[0], numSamples, transport_, blockStart);

    // 5. VocalTrack: input monitoring (mix input → output) and ring-buffer capture.
    //    processBlock uses only atomics and the lock-free RecordBuffer —
    //    no allocation, no mutex, no file I/O.
    vocalTrack_.processBlock(inputChannelData,  numInputChannels,
                              outputChannelData, numOutputChannels,
                              numSamples);

    // 6. Mix ClipPlayer playback into the output.
    //    processBlock uses only atomics and a pre-loaded read-only buffer —
    //    no allocation, no mutex, no file I/O.
    clipPlayer_.processBlock(outputChannelData, numOutputChannels, numSamples);

    // 7. Copy channel 0 (now contains metronome + vocal monitor + clip) to all
    //    additional output channels.
    if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
    {
        for (int ch = 1; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::copy(
                    outputChannelData[ch], outputChannelData[0], numSamples);
    }
}

bool AudioEngine::loadLastTakeForPlayback()
{
    // Grab the most recent take path from the VocalTrack's TakeManager.
    const auto& takes = vocalTrack_.getTakeManager().takes();
    if (takes.empty()) return false;

    const auto& lastTake = takes.back();

    WavReader reader;
    if (!reader.open(lastTake.path)) return false;
    if (reader.samples().empty())    return false;

    // ClipPlayer must be stopped before loading (it uses double-buffer swap).
    clipPlayer_.stop();
    clipPlayer_.seekToStart();
    clipPlayer_.load(reader.samples(), reader.numChannels());
    return true;
}

bool AudioEngine::saveSession(const std::filesystem::path& path)
{
    // Sync TakeManager records → SessionData.
    sessionData_.takes.clear();
    for (const auto& tm : vocalTrack_.getTakeManager().takes())
    {
        TakeEntry entry;
        entry.path         = tm.path.string();
        entry.sampleRate   = tm.sampleRate;
        entry.numChannels  = tm.numChannels;
        entry.isoTimestamp = tm.isoTimestamp;
        sessionData_.takes.push_back(std::move(entry));
    }
    sessionData_.dirty = false;

    const bool ok = SessionDocument::save(path, sessionData_);
    if (ok) SessionDocument::saveRecovery(path, sessionData_);
    return ok;
}

bool AudioEngine::loadSession(const std::filesystem::path& path)
{
    SessionData data;
    if (!SessionDocument::load(path, data)) return false;
    if (!SessionDocument::migrate(data))    return false;

    sessionData_ = std::move(data);
    return true;
}

} // namespace ssbb
