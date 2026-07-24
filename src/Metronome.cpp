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

void Metronome::processBlock(float* output, int numSamples,
                             const Transport& transport,
                             int64_t blockStartSamples) noexcept
{
    // AUDIO THREAD — no allocation, no locks, no logging, no exceptions.

    if (!enabled_.load(std::memory_order_relaxed))
        return;

    const float level = level_.load(std::memory_order_relaxed);
    if (level <= 0.0f)
        return;

    if (!transport.isPlaying())
        return;

    const double bpm       = transport.getTempo();
    const double spb       = Transport::bpmToSamplesPerBeat(bpm, sampleRate_);
    if (spb <= 0.0)
        return;

    const int numerator   = transport.getTimeSigNumerator();
    const int denominator = transport.getTimeSigDenominator();
    if (numerator < 1 || denominator < 1)
        return;

    // One click per denominator note.  The tempo beat unit is a quarter note,
    // so an eighth note (denominator == 8) is half a quarter note, etc.
    const double samplesPerClick = spb * 4.0 / static_cast<double>(denominator);
    if (samplesPerClick <= 0.0)
        return;

    // Detect a loop wrap within this block.
    // wrapOffset: first sample index in the block that belongs to the post-wrap
    //             timeline segment (== numSamples when no wrap occurs).
    // loopStartSamp: timeline sample index the post-wrap segment begins at.
    int     wrapOffset    = numSamples;
    int64_t loopStartSamp = 0;

    if (transport.isLoopEnabled())
    {
        const double  loopStartBeat = transport.getLoopStartBeat();
        const double  loopEndBeat   = transport.getLoopEndBeat();
        const int64_t loopEndSamp   = Transport::beatsToSamples(loopEndBeat, bpm, sampleRate_);
        loopStartSamp               = Transport::beatsToSamples(loopStartBeat, bpm, sampleRate_);

        const int64_t rawEnd = blockStartSamples + static_cast<int64_t>(numSamples);
        if (loopEndSamp > loopStartSamp
            && rawEnd > loopEndSamp
            && blockStartSamples < loopEndSamp)
        {
            wrapOffset = static_cast<int>(loopEndSamp - blockStartSamples);
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // Map sample index to its timeline position, accounting for any loop wrap.
        const double posNow = (i < wrapOffset)
            ? static_cast<double>(blockStartSamples + i)
            : static_cast<double>(loopStartSamp + (i - wrapOffset));

        // Beat-boundary detection: did we cross a click boundary between the
        // previous and current sample?  Using floor-division so beat 0 is
        // triggered on the very first sample of playback (posPrev == -1 yields
        // beatPrev == -1, beatNow == 0 → trigger).
        const double posPrev = posNow - 1.0;

        const auto beatNow  = static_cast<int64_t>(std::floor(posNow  / samplesPerClick));
        const auto beatPrev = static_cast<int64_t>(std::floor(posPrev / samplesPerClick));

        if (beatNow != beatPrev)
        {
            // Accent on the first click of each bar (beat index 0 mod numerator).
            // Positive-safe modulo handles negative beat indices near timeline start.
            const int mod = static_cast<int>(
                ((beatNow % static_cast<int64_t>(numerator)) +
                  static_cast<int64_t>(numerator)) %
                  static_cast<int64_t>(numerator));
            triggerClick(mod == 0);
        }

        // Mix (not assign) so future audio tracks can be summed into the same buffer.
        output[i] += nextSample() * level;
    }
}

} // namespace ssbb
