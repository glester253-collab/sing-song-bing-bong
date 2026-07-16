// Transport.cpp
// Audio-thread rules: process() must remain allocation-free, lock-free,
// exception-free, and free of file/network/logging/AI work.
// The static math helpers are pure functions with no side-effects.
#include "Transport.h"

#include <algorithm>   // std::clamp
#include <cmath>       // std::floor (not used here but kept for completeness)
#include <cstdint>

namespace ssbb {

// ---- Message-thread setters ----

void Transport::setTempo(double bpm) noexcept
{
    tempo_.store(std::clamp(bpm, 20.0, 300.0), std::memory_order_relaxed);
}

void Transport::setTimeSignature(int numerator, int denominator) noexcept
{
    timeSigNumerator_.store(numerator,   std::memory_order_relaxed);
    timeSigDenominator_.store(denominator, std::memory_order_relaxed);
}

void Transport::setLoopRange(double startBeat, double endBeat) noexcept
{
    loopStartBeat_.store(startBeat, std::memory_order_relaxed);
    loopEndBeat_.store(endBeat,     std::memory_order_relaxed);
}

void Transport::setLoopEnabled(bool enabled) noexcept
{
    loopEnabled_.store(enabled, std::memory_order_relaxed);
}

void Transport::play() noexcept
{
    playing_.store(true, std::memory_order_relaxed);
}

void Transport::stop() noexcept
{
    playing_.store(false, std::memory_order_relaxed);
}

void Transport::setPositionInBeats(double beats) noexcept
{
    const double bpm = tempo_.load(std::memory_order_relaxed);
    positionInSamples_.store(beatsToSamples(beats, bpm, sampleRate_),
                             std::memory_order_relaxed);
}

// ---- Audio-thread API ----

void Transport::prepare(double sampleRate, int /*blockSize*/) noexcept
{
    // Called on the message thread before the audio device starts.
    // Safe to write sampleRate_ here because the audio callback is not yet running.
    sampleRate_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
}

void Transport::process(int numSamples) noexcept
{
    // AUDIO THREAD — no allocation, no locks, no logging, no exceptions.
    if (!playing_.load(std::memory_order_relaxed))
        return;

    int64_t pos = positionInSamples_.load(std::memory_order_relaxed);
    pos += static_cast<int64_t>(numSamples);

    if (loopEnabled_.load(std::memory_order_relaxed))
    {
        const double  bpm        = tempo_.load(std::memory_order_relaxed);
        const double  loopStart  = loopStartBeat_.load(std::memory_order_relaxed);
        const double  loopEnd    = loopEndBeat_.load(std::memory_order_relaxed);
        const int64_t startSamp  = beatsToSamples(loopStart, bpm, sampleRate_);
        const int64_t endSamp    = beatsToSamples(loopEnd,   bpm, sampleRate_);

        if (endSamp > startSamp && pos >= endSamp)
        {
            const int64_t loopLen = endSamp - startSamp;
            pos = startSamp + ((pos - startSamp) % loopLen);
        }
    }

    positionInSamples_.store(pos, std::memory_order_relaxed);
}

// ---- Pure-math utilities ----

double Transport::beatsToSeconds(double beats, double bpm) noexcept
{
    if (bpm <= 0.0) return 0.0;
    return beats * 60.0 / bpm;
}

int64_t Transport::beatsToSamples(double beats, double bpm, double sampleRate) noexcept
{
    if (bpm <= 0.0 || sampleRate <= 0.0) return 0;
    return static_cast<int64_t>(beats * 60.0 / bpm * sampleRate);
}

double Transport::samplesToBeats(int64_t samples, double bpm, double sampleRate) noexcept
{
    if (bpm <= 0.0 || sampleRate <= 0.0) return 0.0;
    return static_cast<double>(samples) / sampleRate * bpm / 60.0;
}

double Transport::bpmToSamplesPerBeat(double bpm, double sampleRate) noexcept
{
    if (bpm <= 0.0 || sampleRate <= 0.0) return 0.0;
    return sampleRate * 60.0 / bpm;
}

} // namespace ssbb
