#pragma once
#include <atomic>
#include <cstdint>

namespace ssbb {

class Transport;   // forward declaration — avoids pulling in Transport.h from callers
                   // that only see Metronome; Transport.cpp includes Transport.h directly.

/// Metronome: generates click audio entirely in the audio callback.
/// No allocation, no locks, no logging, no exceptions inside processBlock.
///
/// Click model: 10 ms linear-decay sine burst.
///   Accent (downbeat): 1000 Hz, amplitude 0.8
///   Normal beat:        800 Hz, amplitude 0.5
class Metronome
{
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const noexcept        { return enabled_.load(std::memory_order_relaxed); }

    /// Audio thread: mix metronome clicks into output[0..numSamples-1].
    /// transport.process() must have already advanced the position for this block
    /// before this method is called (see AudioEngine).
    void processBlock(float* output, int numSamples, const Transport& transport) noexcept;

private:
    void  triggerClick(bool accented) noexcept;
    float nextSample() noexcept;

    double sampleRate_      { 44100.0 };
    int    clickLengthSamp_ { 0 };
    double oscPhase_        { 0.0 };
    double oscPhaseInc_     { 0.0 };
    int    clickPhase_      { -1 };   // -1 = idle
    bool   accented_        { false };

    std::atomic<bool> enabled_ { false };
};

} // namespace ssbb
