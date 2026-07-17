#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace ssbb {
class BeatEngine
{
public:
    void prepare(double sampleRate, int maximumBlockSize) noexcept;
    void render(float* const* outputs, int channels, int samples) noexcept;
    void setTempo(double bpm) noexcept;
    void setStep(int lane, int step, bool enabled) noexcept;
    bool getStep(int lane, int step) const noexcept;
    void loadWestCoastBounce() noexcept;
    void loadBoomBap() noexcept;
    void reset() noexcept;
private:
    std::array<std::array<std::atomic_bool, 16>, 3> pattern_ {};
    std::atomic<double> tempo_ { 96.0 };
    double sampleRate_ = 48000.0, samplesUntilStep_ = 0.0;
    int step_ = 0, kickAge_ = -1, snareAge_ = -1, hatAge_ = -1;
    uint32_t noiseState_ = 0x12345678u;
    float noise() noexcept;
};
} // namespace ssbb
