#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include "Transport.h"
#include "Metronome.h"

namespace ssbb {

/// AudioEngine owns the JUCE AudioDeviceManager and bridges it to our
/// lock-free Transport and Metronome.
///
/// Ownership model:
///   - AudioEngine is created on the message thread before MainWindow.
///   - The AudioIODeviceCallback methods are called on the audio thread.
///   - All public getters (getTransport, getMetronome, etc.) are safe to
///     call from any thread — they return references to objects whose
///     audio-thread-visible state is protected by atomics.
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager_; }
    Transport&  getTransport()  noexcept { return transport_; }
    Metronome&  getMetronome()  noexcept { return metronome_; }

    /// Number of active input channels reported at the last audioDeviceAboutToStart.
    int getNumInputChannels() const noexcept
    {
        return numInputChannels_.load(std::memory_order_relaxed);
    }
    /// Number of active output channels reported at the last audioDeviceAboutToStart.
    int getNumOutputChannels() const noexcept
    {
        return numOutputChannels_.load(std::memory_order_relaxed);
    }
    /// Round-trip latency (input + output) in milliseconds.
    double getEstimatedLatencyMs() const noexcept
    {
        return estimatedLatencyMs_.load(std::memory_order_relaxed);
    }

    // juce::AudioIODeviceCallback overrides
    void audioDeviceIOCallbackWithContext(
        const float* const*                      inputChannelData,
        int                                      numInputChannels,
        float* const*                            outputChannelData,
        int                                      numOutputChannels,
        int                                      numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

private:
    juce::AudioDeviceManager deviceManager_;
    Transport  transport_;
    Metronome  metronome_;

    std::atomic<int>    numInputChannels_   { 0 };
    std::atomic<int>    numOutputChannels_  { 0 };
    std::atomic<double> estimatedLatencyMs_ { 0.0 };
};

} // namespace ssbb
