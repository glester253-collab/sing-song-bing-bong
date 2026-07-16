// Metronome.cpp
// Audio-thread rules enforced in processBlock/nextSample/triggerClick:
//   no allocation, no locks, no logging, no exceptions, no JUCE API calls.
#include "Metronome.h"
#include "Transport.h"   // full definition needed to call Transport methods

#include <cmath>         // std::sin, std::floor, std::round

namespace ssbb {

namespace {
    // Avoid non-standard M_PI
    constexpr double kPi = 3.14159265358979323846;
} // namespace

// ---- Message-thread / setup ----

void Metronome::prepare(double sampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate_      = (sampleRate > 0.0) ? sampleRate : 44100.0;
    clickLengthSamp_ = static_cast<int>(std::round(0.010 * sampleRate_));  // 10 ms
    oscPhase_        = 0.0;
    oscPhaseInc_     = 0.0;
    clickPhase_      = -1;
    accented_        = false;
}

// ---- Audio-thread helpers ----

void Metronome::triggerClick(bool accented) noexcept
{
    // Called from processBlock — audio thread, no allocation, no locks.
    accented_    = accented;
    clickPhase_  = 0;
    oscPhase_    = 0.0;
    const double freq = accented ? 1000.0 : 800.0;
    oscPhaseInc_ = 2.0 * kPi * freq / sampleRate_;
}

float Metronome::nextSample() noexcept
{
    // Called from processBlock — audio thread.
    if (clickPhase_ < 0)
        return 0.0f;

    if (clickPhase_ >= clickLengthSamp_)
    {
        clickPhase_ = -1;
        return 0.0f;
    }

    const float amplitude = accented_ ? 0.8f : 0.5f;
    // Linear decay: full amplitude at phase 0, zero at phase clickLengthSamp_
    const float decay  = 1.0f - static_cast<float>(clickPhase_) /
                                 static_cast<float>(clickLengthSamp_);
    const float sample = amplitude * decay * static_cast<float>(std::sin(oscPhase_));

    oscPhase_ += oscPhaseInc_;
    ++clickPhase_;

    return sample;
}

// ---- Audio-thread main entry point ----

void Metronome::processBlock(float* output, int numSamples, const Transport& transport) noexcept
{
    // AUDIO THREAD — no allocation, no locks, no logging, no exceptions.

    if (!enabled_.load(std::memory_order_relaxed))
        return;

    if (!transport.isPlaying())
        return;

    const double bpm = transport.getTempo();
    const double spb = Transport::bpmToSamplesPerBeat(bpm, sampleRate_);

    if (spb <= 0.0)
        return;

    const int     numerator  = transport.getTimeSigNumerator();
    // transport.process() has already advanced position by numSamples this block,
    // so blockStart is the sample index at which this block began.
    const int64_t blockStart = transport.getPositionInSamples() -
                               static_cast<int64_t>(numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        // Detect a beat boundary crossing between sample (i-1) and sample i.
        // When i == 0 this correctly catches a beat at the very first sample
        // of the block (e.g., the very first block after play()).
        const double posNow  = static_cast<double>(blockStart + i);
        const double posPrev = posNow - 1.0;

        const auto beatNow  = static_cast<int64_t>(std::floor(posNow  / spb));
        const auto beatPrev = static_cast<int64_t>(std::floor(posPrev / spb));

        if (beatNow != beatPrev)
        {
            // Positive-safe modulo for accent detection; beatNow can theoretically
            // be negative near the start of playback if blockStart wrapped.
            const int mod = static_cast<int>(
                ((beatNow % static_cast<int64_t>(numerator)) +
                  static_cast<int64_t>(numerator)) %
                  static_cast<int64_t>(numerator));
            triggerClick(mod == 0);
        }

        // Mix (not assign) so future audio tracks can be summed into the same buffer.
        output[i] += nextSample();
    }
}

} // namespace ssbb
