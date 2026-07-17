#include "BeatEngine.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ssbb {
void BeatEngine::prepare(double sr, int) noexcept { sampleRate_ = std::max(1.0, sr); reset(); }
void BeatEngine::setTempo(double bpm) noexcept { tempo_.store(std::clamp(bpm, 50.0, 220.0), std::memory_order_relaxed); }
void BeatEngine::reset() noexcept { samplesUntilStep_ = 0.0; step_ = 0; kickAge_ = snareAge_ = hatAge_ = -1; }
void BeatEngine::setStep(int lane, int step, bool value) noexcept { if (lane >= 0 && lane < 3 && step >= 0 && step < 16) pattern_[static_cast<size_t>(lane)][static_cast<size_t>(step)].store(value, std::memory_order_relaxed); }
bool BeatEngine::getStep(int lane, int step) const noexcept { return lane >= 0 && lane < 3 && step >= 0 && step < 16 && pattern_[static_cast<size_t>(lane)][static_cast<size_t>(step)].load(std::memory_order_relaxed); }
void BeatEngine::loadWestCoastBounce() noexcept { for (auto& lane : pattern_) for (auto& s : lane) s.store(false); for (int s : {0, 6, 10}) setStep(0, s, true); for (int s : {4, 12}) setStep(1, s, true); for (int s = 0; s < 16; s += 2) setStep(2, s, true); setStep(2, 15, true); }
void BeatEngine::loadBoomBap() noexcept { for (auto& lane : pattern_) for (auto& s : lane) s.store(false); for (int s : {0, 7, 10}) setStep(0, s, true); for (int s : {4, 12}) setStep(1, s, true); for (int s = 0; s < 16; s += 2) setStep(2, s, true); }
float BeatEngine::noise() noexcept { noiseState_ ^= noiseState_ << 13; noiseState_ ^= noiseState_ >> 17; noiseState_ ^= noiseState_ << 5; return static_cast<float>(noiseState_ & 0xffffu) / 32767.5f - 1.0f; }
void BeatEngine::render(float* const* outputs, int channels, int samples) noexcept
{
    const double samplesPerStep = sampleRate_ * 60.0 / tempo_.load(std::memory_order_relaxed) / 4.0;
    for (int i = 0; i < samples; ++i)
    {
        if (samplesUntilStep_ <= 0.0) { if (getStep(0, step_)) kickAge_ = 0; if (getStep(1, step_)) snareAge_ = 0; if (getStep(2, step_)) hatAge_ = 0; step_ = (step_ + 1) & 15; samplesUntilStep_ += samplesPerStep; }
        float value = 0.0f;
        if (kickAge_ >= 0) { const double t = kickAge_++ / sampleRate_; value += 0.72f * static_cast<float>(std::exp(-18.0 * t) * std::sin(2.0 * std::numbers::pi * (56.0 - 20.0 * t) * t)); if (t > 0.45) kickAge_ = -1; }
        if (snareAge_ >= 0) { const double t = snareAge_++ / sampleRate_; value += 0.28f * noise() * static_cast<float>(std::exp(-24.0 * t)); if (t > 0.28) snareAge_ = -1; }
        if (hatAge_ >= 0) { const double t = hatAge_++ / sampleRate_; value += 0.08f * noise() * static_cast<float>(std::exp(-90.0 * t)); if (t > 0.08) hatAge_ = -1; }
        if (!std::isfinite(value)) value = 0.0f;
        for (int ch = 0; ch < channels; ++ch) if (outputs[ch] != nullptr) outputs[ch][i] += value;
        samplesUntilStep_ -= 1.0;
    }
}
} // namespace ssbb
