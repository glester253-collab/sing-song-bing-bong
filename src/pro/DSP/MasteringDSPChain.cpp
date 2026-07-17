#include "MasteringDSPChain.h"
#include <cmath>

namespace ssbb {
void MasteringDSPChain::prepare(const juce::dsp::ProcessSpec& spec) { comp_.prepare(spec); limiter_.prepare(spec); comp_.setThreshold(-12.0f); comp_.setRatio(1.6f); comp_.setAttack(25.0f); comp_.setRelease(180.0f); limiter_.setThreshold(-0.8f); limiter_.setRelease(80.0f); }
void MasteringDSPChain::process(juce::AudioBuffer<float>& buffer) noexcept
{
    juce::ScopedNoDenormals guard; juce::dsp::AudioBlock<float> block(buffer); juce::dsp::ProcessContextReplacing<float> context(block); comp_.process(context);
    const float drive = 1.0f + 1.5f * warmth_.load();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) for (int i = 0; i < buffer.getNumSamples(); ++i) buffer.setSample(ch, i, std::tanh(buffer.getSample(ch, i) * drive) / std::tanh(drive));
    if (buffer.getNumChannels() >= 2) { const float amount = 1.0f + 0.9f * width_.load(); for (int i = 0; i < buffer.getNumSamples(); ++i) { const float l = buffer.getSample(0, i), r = buffer.getSample(1, i), mid = 0.5f * (l + r), side = 0.5f * (l - r) * amount; buffer.setSample(0, i, mid + side); buffer.setSample(1, i, mid - side); } }
    buffer.applyGain(juce::Decibels::decibelsToGain(juce::jmap(loudness_.load(), 0.0f, 1.0f, -6.0f, 2.0f))); limiter_.process(context);
}
} // namespace ssbb
