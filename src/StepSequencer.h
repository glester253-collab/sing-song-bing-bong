#pragma once
// StepSequencer.h — 16/32/64-step drum sequencer with swing, probability,
//                   and per-step velocity.
//
// THREAD MODEL:
//   Message thread : all configuration (steps, patterns, swing).
//   Audio thread   : process() emits PadHitEvent(s) via a callback when a
//                    step fires.  NO allocation, NO locks, NO I/O.
//
// Swing:
//   Even-numbered steps are played on-beat; odd-numbered steps are delayed by
//   (swingAmount * stepDuration * 0.5) samples.  swingAmount=0 is straight;
//   swingAmount=1 pushes the off-beat fully to the next on-beat (triplet feel).
//
// Probability:
//   Each step has a trigger probability 0.0–1.0.  The sequencer uses a
//   deterministic PRNG seeded per-pattern so playback is reproducible.

#include "DrumPad.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>

namespace ssbb {

class StepSequencer
{
public:
    static constexpr int kMaxSteps = 64;
    static constexpr int kNumPads  = DrumPad::kNumPads; // 16 pads

    /// Callback invoked on the audio thread when a step fires.
    using HitCallback = std::function<void(int padIndex, int velocity, int64_t sampleTime)>;

    // ---- Step data model -------------------------------------------------

    struct Step
    {
        bool  active      { false }; ///< Is this step on?
        int   velocity    { 100 };   ///< Per-step velocity override (0–127).
        float probability { 1.0f };  ///< Probability the step fires (0–1).
    };

    // ---- Constructor -----------------------------------------------------

    StepSequencer()
    {
        for (auto& row : steps_)
            row.fill(Step{});
    }

    // Non-copyable / non-movable (atomics).
    StepSequencer(const StepSequencer&)            = delete;
    StepSequencer& operator=(const StepSequencer&) = delete;
    StepSequencer(StepSequencer&&)                 = delete;
    StepSequencer& operator=(StepSequencer&&)      = delete;

    // ---- Prepare (device setup thread) -----------------------------------

    void prepare(double sampleRate, int /*blockSize*/) noexcept
    {
        sampleRate_.store(sampleRate > 0.0 ? sampleRate : 44100.0,
                          std::memory_order_relaxed);
        recalcStepSamples();
    }

    // ---- Message-thread configuration ------------------------------------

    void setNumSteps(int n) noexcept
    {
        if (n >= 1 && n <= kMaxSteps)
            numSteps_.store(n, std::memory_order_relaxed);
    }

    int getNumSteps() const noexcept
    {
        return numSteps_.load(std::memory_order_relaxed);
    }

    void setTempo(double bpm) noexcept
    {
        if (bpm > 0.0)
        {
            tempo_.store(bpm, std::memory_order_relaxed);
            recalcStepSamples();
        }
    }

    /// Swing amount 0.0 (straight) to 1.0 (full triplet).
    void setSwing(float swing) noexcept
    {
        if (swing < 0.0f) swing = 0.0f;
        if (swing > 1.0f) swing = 1.0f;
        swing_.store(swing, std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) noexcept
    {
        enabled_.store(enabled, std::memory_order_relaxed);
    }

    bool isEnabled() const noexcept
    {
        return enabled_.load(std::memory_order_relaxed);
    }

    /// Set or clear a step on a specific pad row.
    void setStep(int padIndex, int stepIndex, const Step& step)
    {
        if (padIndex  < 0 || padIndex  >= kNumPads)  return;
        if (stepIndex < 0 || stepIndex >= kMaxSteps) return;
        steps_[static_cast<std::size_t>(padIndex)]
              [static_cast<std::size_t>(stepIndex)] = step;
    }

    Step getStep(int padIndex, int stepIndex) const
    {
        if (padIndex  < 0 || padIndex  >= kNumPads)  return {};
        if (stepIndex < 0 || stepIndex >= kMaxSteps) return {};
        return steps_[static_cast<std::size_t>(padIndex)]
                     [static_cast<std::size_t>(stepIndex)];
    }

    /// Register the callback that receives pad-hit events.
    /// Must be set before process() is called.
    void setHitCallback(HitCallback cb) { hitCallback_ = std::move(cb); }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Advance the sequencer by `numSamples` at transport position `startSample`.
    /// Fires the hitCallback for each step that falls within this block.
    ///
    /// @param startSample  Absolute transport position at the start of the block.
    /// @param numSamples   Block size.
    /// @param isPlaying    True if transport is running.
    void process(int64_t startSample,
                 int     numSamples,
                 bool    isPlaying) noexcept
    {
        if (!isPlaying || !enabled_.load(std::memory_order_relaxed))
            return;
        if (!hitCallback_) return;

        const int    n         = numSteps_.load(std::memory_order_relaxed);
        const double stepLen   = stepSamples_.load(std::memory_order_relaxed);
        const float  swing     = swing_.load(std::memory_order_relaxed);

        if (stepLen <= 0.0 || n <= 0) return;

        const double patternLen = stepLen * static_cast<double>(n);
        const int64_t blockEnd  = startSample + static_cast<int64_t>(numSamples);

        for (int step = 0; step < n; ++step)
        {
            // Compute the on-beat position of this step within the looping pattern.
            const double beatPos = stepLen * static_cast<double>(step);

            // Apply swing: odd steps are pushed back.
            const double swingOffset = (step % 2 != 0)
                                       ? stepLen * 0.5 * static_cast<double>(swing)
                                       : 0.0;
            const double stepOnset = beatPos + swingOffset;

            // Find which pattern cycle(s) overlap this block.
            // The transport position mod patternLen gives offset within the pattern.
            const int64_t cycleNumber = static_cast<int64_t>(
                static_cast<double>(startSample) / patternLen);

            for (int64_t cycle = cycleNumber - 1; cycle <= cycleNumber + 1; ++cycle)
            {
                const double cycleStart = static_cast<double>(cycle) * patternLen;
                const int64_t fireAt = static_cast<int64_t>(cycleStart + stepOnset);

                if (fireAt < startSample || fireAt >= blockEnd)
                    continue;

                // Fire for each pad row that has this step active.
                for (int pad = 0; pad < kNumPads; ++pad)
                {
                    const Step& s = steps_[static_cast<std::size_t>(pad)]
                                          [static_cast<std::size_t>(step)];
                    if (!s.active) continue;

                    // Probability check using a lightweight LCG PRNG.
                    if (s.probability < 1.0f)
                    {
                        const uint32_t hash = lcg(static_cast<uint32_t>(fireAt)
                                                  ^ static_cast<uint32_t>(pad * 31));
                        const float rnd = static_cast<float>(hash >> 8) /
                                          static_cast<float>(1u << 24u);
                        if (rnd >= s.probability) continue;
                    }

                    hitCallback_(pad, s.velocity, fireAt);
                }
            }
        }
    }

    /// Reset the sequencer to step 0 (call on transport rewind).
    void reset() noexcept {}  // position is derived from transport; nothing to reset.

private:
    // ---- Step pattern storage (message thread) ---------------------------
    using PadRow = std::array<Step, kMaxSteps>;
    std::array<PadRow, kNumPads> steps_;

    // ---- Configuration atomics -------------------------------------------
    std::atomic<int>    numSteps_    { 16 };
    std::atomic<double> tempo_       { 120.0 };
    std::atomic<double> stepSamples_ { 0.0 };   // samples per step (1/16-note)
    std::atomic<float>  swing_       { 0.0f };
    std::atomic<bool>   enabled_     { false };
    std::atomic<double> sampleRate_  { 44100.0 };

    HitCallback hitCallback_;

    void recalcStepSamples() noexcept
    {
        const double sr   = sampleRate_.load(std::memory_order_relaxed);
        const double bpm  = tempo_.load(std::memory_order_relaxed);
        // 1 beat = 60/bpm seconds; 1 sixteenth = 1/4 beat.
        const double step = (bpm > 0.0) ? (sr * 60.0 / bpm / 4.0) : 0.0;
        stepSamples_.store(step, std::memory_order_relaxed);
    }

    // Minimal LCG for probability checks — no allocation, no locks.
    static constexpr uint32_t lcg(uint32_t x) noexcept
    {
        return x * 1664525u + 1013904223u;
    }
};

} // namespace ssbb
