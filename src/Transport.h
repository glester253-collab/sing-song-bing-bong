#pragma once
#include <atomic>
#include <cstdint>

namespace ssbb {

/// Thread-safe transport state.
/// Setters run on the message thread; process() runs on the audio thread.
/// Only atomics are shared between threads — no locks, no allocation in process().
class Transport
{
public:
    // ---- Message-thread API ----
    void setTempo(double bpm) noexcept;
    void setTimeSignature(int numerator, int denominator) noexcept;
    void setLoopRange(double startBeat, double endBeat) noexcept;
    void setLoopEnabled(bool enabled) noexcept;
    void play() noexcept;
    void stop() noexcept;
    void setPositionInBeats(double beats) noexcept;

    double getTempo()              const noexcept { return tempo_.load(std::memory_order_relaxed); }
    bool   isPlaying()             const noexcept { return playing_.load(std::memory_order_relaxed); }
    int    getTimeSigNumerator()   const noexcept { return timeSigNumerator_.load(std::memory_order_relaxed); }
    int    getTimeSigDenominator() const noexcept { return timeSigDenominator_.load(std::memory_order_relaxed); }
    bool   isLoopEnabled()         const noexcept { return loopEnabled_.load(std::memory_order_relaxed); }
    double getLoopStartBeat()      const noexcept { return loopStartBeat_.load(std::memory_order_relaxed); }
    double getLoopEndBeat()        const noexcept { return loopEndBeat_.load(std::memory_order_relaxed); }

    // ---- Audio-thread API: no allocation, no locks, no logging ----
    void prepare(double sampleRate, int blockSize) noexcept;
    void process(int numSamples) noexcept;

    int64_t getPositionInSamples() const noexcept
    {
        return positionInSamples_.load(std::memory_order_relaxed);
    }
    double getSampleRate() const noexcept { return sampleRate_; }

    // ---- Pure-math utilities (testable without JUCE) ----
    static double  beatsToSeconds(double beats, double bpm) noexcept;
    static int64_t beatsToSamples(double beats, double bpm, double sampleRate) noexcept;
    static double  samplesToBeats(int64_t samples, double bpm, double sampleRate) noexcept;
    static double  bpmToSamplesPerBeat(double bpm, double sampleRate) noexcept;

private:
    std::atomic<double>  tempo_              { 120.0 };
    std::atomic<bool>    playing_            { false };
    std::atomic<int>     timeSigNumerator_   { 4 };
    std::atomic<int>     timeSigDenominator_ { 4 };
    std::atomic<double>  loopStartBeat_      { 0.0 };
    std::atomic<double>  loopEndBeat_        { 16.0 };
    std::atomic<bool>    loopEnabled_        { false };
    std::atomic<int64_t> positionInSamples_  { 0 };

    // Audio-thread-only: written only in prepare(), read only in process().
    // Not accessed from any other thread; no atomic needed.
    double sampleRate_ { 44100.0 };
};

} // namespace ssbb
