#pragma once
#include <juce_dsp/juce_dsp.h>

namespace ssbb {
class VocalDSPChain
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset() noexcept;
    void process(juce::AudioBuffer<float>& buffer) noexcept;
private:
    juce::dsp::Compressor<float> comp_;
    juce::dsp::IIR::Filter<float> low_;
    juce::dsp::IIR::Filter<float> high_;
    juce::dsp::Reverb reverb_;
    juce::dsp::DelayLine<float> delay_ { 192000 };
    juce::dsp::Limiter<float> limiter_;
    float delaySamples_ = 4080.0f;
};
} // namespace ssbb
