#include "MemoryRecorder.h"
#include <algorithm>
namespace ssbb {
void MemoryRecorder::start(int channels, int samples) { recording_.store(false); buffer_.setSize(std::max(1, channels), std::max(1, samples)); buffer_.clear(); writePosition_.store(0); recording_.store(true); }
void MemoryRecorder::process(const juce::AudioBuffer<float>& input) noexcept { if (!recording_.load()) return; const int pos = writePosition_.load(), count = std::min(input.getNumSamples(), buffer_.getNumSamples() - pos); if (count <= 0) { recording_.store(false); return; } for (int ch = 0; ch < std::min(input.getNumChannels(), buffer_.getNumChannels()); ++ch) buffer_.copyFrom(ch, pos, input, ch, 0, count); writePosition_.store(pos + count); if (pos + count == buffer_.getNumSamples()) recording_.store(false); }
} // namespace ssbb
