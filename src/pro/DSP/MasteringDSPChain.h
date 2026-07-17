#pragma once
#include <juce_dsp/juce_dsp.h>
#include <atomic>

namespace ssbb {
class MasteringDSPChain
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void process(juce::AudioBuffer<float>& buffer) noexcept;
    void setLoudness(float value) noexcept { loudness_.store(value); }
    void setWidth(float value) noexcept { width_.store(value); }
    void setWarmth(float value) noexcept { warmth_.store(value); }
private:
    juce::dsp::Compressor<float> comp_;
    juce::dsp::Limiter<float> limiter_;
    std::atomic<float> loudness_ { 0.65f }, width_ { 0.2f }, warmth_ { 0.2f };
};
} // namespace ssbb
