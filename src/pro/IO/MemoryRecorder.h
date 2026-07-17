#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace ssbb {
class MemoryRecorder
{
public:
    void start(int channels, int samples);
    void stop() noexcept { recording_.store(false); }
    void process(const juce::AudioBuffer<float>& input) noexcept;
    const juce::AudioBuffer<float>& recorded() const noexcept { return buffer_; }
private:
    juce::AudioBuffer<float> buffer_;
    std::atomic<bool> recording_ { false };
    std::atomic<int> writePosition_ { 0 };
};
} // namespace ssbb
