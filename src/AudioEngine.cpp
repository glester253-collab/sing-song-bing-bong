// AudioEngine.cpp
// Message-thread methods (constructor, audioDeviceAboutToStart,
// audioDeviceError, audioDeviceStopped) may use JUCE APIs freely.
//
// audioDeviceIOCallbackWithContext is the AUDIO THREAD:
//   NO allocation, NO locks, NO logging, NO JUCE String construction,
//   NO file/network I/O, NO exceptions, NO AI work.
#include "AudioEngine.h"

namespace ssbb {

AudioEngine::AudioEngine()
{
    // initialise(maxInputChannels, maxOutputChannels, savedState, selectDefaultDevice)
    // Using explicit form rather than initialiseWithDefaultDevices so callers
    // can later supply a saved XmlElement for recall.
    deviceManager_.initialise(2, 2, nullptr, true);
    deviceManager_.addAudioCallback(this);
}

AudioEngine::~AudioEngine()
{
    deviceManager_.removeAudioCallback(this);
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

void AudioEngine::audioDeviceError(const juce::String& errorMessage)
{
    // Stop the transport so the UI reflects the error state.
    transport_.stop();
    // DBG is only safe here if this callback is on the message thread.
    // On some platforms it may be called from a device thread.  The transport
    // stop above is the only audio-state change we make; the log is best-effort.
    DBG("AudioEngine error: " << errorMessage);
}

// ---- AUDIO THREAD ----

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* /*inputChannelData*/,
    int          /*numInputChannels*/,
    float* const* outputChannelData,
    int           numOutputChannels,
    int           numSamples,
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

    // 2. Advance the transport position (atomic store, no allocation).
    transport_.process(numSamples);

    // 3. Mix metronome clicks into channel 0.
    if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
        metronome_.processBlock(outputChannelData[0], numSamples, transport_);

    // 4. Copy mono metronome signal to all additional output channels.
    for (int ch = 1; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::copy(
                outputChannelData[ch], outputChannelData[0], numSamples);
}

} // namespace ssbb
