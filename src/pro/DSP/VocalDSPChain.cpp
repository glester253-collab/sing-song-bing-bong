#include "VocalDSPChain.h"

namespace ssbb {
void VocalDSPChain::prepare(const juce::dsp::ProcessSpec& spec)
{
    comp_.prepare(spec); low_.prepare(spec); high_.prepare(spec); reverb_.prepare(spec); delay_.prepare(spec); limiter_.prepare(spec);
    comp_.setThreshold(-18.0f); comp_.setRatio(3.0f); comp_.setAttack(8.0f); comp_.setRelease(90.0f);
    low_.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(spec.sampleRate, 85.0);
    high_.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf(spec.sampleRate, 6500.0, 0.75f, 1.18f);
    juce::dsp::Reverb::Parameters p; p.roomSize = 0.2f; p.damping = 0.55f; p.wetLevel = 0.08f; p.dryLevel = 0.92f; p.width = 0.8f; reverb_.setParameters(p);
    delaySamples_ = static_cast<float>(spec.sampleRate * 0.085); delay_.setDelay(delaySamples_);
    limiter_.setThreshold(-1.0f); limiter_.setRelease(60.0f); reset();
}
void VocalDSPChain::reset() noexcept { comp_.reset(); low_.reset(); high_.reset(); reverb_.reset(); delay_.reset(); limiter_.reset(); }
void VocalDSPChain::process(juce::AudioBuffer<float>& buffer) noexcept
{
    juce::ScopedNoDenormals guard;
    juce::dsp::AudioBlock<float> block(buffer); juce::dsp::ProcessContextReplacing<float> context(block);
    comp_.process(context); low_.process(context); high_.process(context); reverb_.process(context);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i) { const float dry = buffer.getSample(ch, i); const float echo = delay_.popSample(ch); delay_.pushSample(ch, dry); buffer.setSample(ch, i, dry + 0.13f * echo); }
    limiter_.process(context);
}
} // namespace ssbb
