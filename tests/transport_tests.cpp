// transport_tests.cpp
// Plain C++20 — NO JUCE headers, NO third-party test frameworks.
// Links only Transport.cpp so the test binary has zero JUCE dependency.
// Follow the same expect() pattern established in bootstrap_tests.cpp.
#include "Transport.h"       // ssbb::Transport — includes only <atomic> and <cstdint>

#include <cmath>             // std::abs, std::floor
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message, bool& success)
{
    if (!condition)
    {
        std::cerr << "[FAIL] " << message << '\n';
        success = false;
    }
}

// Floating-point equality with a small absolute epsilon.
constexpr double kEps = 1e-6;

bool nearEq(double a, double b)
{
    return std::abs(a - b) < kEps;
}

} // namespace

int main()
{
    bool success = true;

    using T = ssbb::Transport;

    // ------------------------------------------------------------------ //
    // beatsToSeconds                                                       //
    // ------------------------------------------------------------------ //
    expect(nearEq(T::beatsToSeconds(1.0, 120.0), 0.5),
           "beatsToSeconds(1.0, 120.0) → 0.5", success);

    expect(nearEq(T::beatsToSeconds(4.0, 60.0), 4.0),
           "beatsToSeconds(4.0, 60.0) → 4.0", success);

    expect(nearEq(T::beatsToSeconds(0.0, 120.0), 0.0),
           "beatsToSeconds(0.0, 120.0) → 0.0", success);

    expect(nearEq(T::beatsToSeconds(1.0, 0.0), 0.0),
           "beatsToSeconds(1.0, 0.0) guard → 0.0", success);

    // ------------------------------------------------------------------ //
    // beatsToSamples                                                       //
    // ------------------------------------------------------------------ //
    expect(T::beatsToSamples(1.0, 120.0, 44100.0) == 22050,
           "beatsToSamples(1.0, 120.0, 44100.0) → 22050", success);

    expect(T::beatsToSamples(0.0, 120.0, 44100.0) == 0,
           "beatsToSamples(0.0, 120.0, 44100.0) → 0", success);

    // Guard: invalid bpm
    expect(T::beatsToSamples(1.0, 0.0, 44100.0) == 0,
           "beatsToSamples(1.0, 0.0, 44100.0) guard → 0", success);

    // Guard: invalid sample rate
    expect(T::beatsToSamples(1.0, 120.0, 0.0) == 0,
           "beatsToSamples(1.0, 120.0, 0.0) guard → 0", success);

    // ------------------------------------------------------------------ //
    // samplesToBeats                                                       //
    // ------------------------------------------------------------------ //
    expect(nearEq(T::samplesToBeats(22050, 120.0, 44100.0), 1.0),
           "samplesToBeats(22050, 120.0, 44100.0) → 1.0", success);

    expect(nearEq(T::samplesToBeats(0, 120.0, 44100.0), 0.0),
           "samplesToBeats(0, 120.0, 44100.0) → 0.0", success);

    // Guard: invalid bpm
    expect(nearEq(T::samplesToBeats(22050, 0.0, 44100.0), 0.0),
           "samplesToBeats(22050, 0.0, 44100.0) guard → 0.0", success);

    // Guard: invalid sample rate
    expect(nearEq(T::samplesToBeats(22050, 120.0, 0.0), 0.0),
           "samplesToBeats(22050, 120.0, 0.0) guard → 0.0", success);

    // ------------------------------------------------------------------ //
    // bpmToSamplesPerBeat                                                  //
    // ------------------------------------------------------------------ //
    expect(nearEq(T::bpmToSamplesPerBeat(120.0, 44100.0), 22050.0),
           "bpmToSamplesPerBeat(120.0, 44100.0) → 22050.0", success);

    expect(nearEq(T::bpmToSamplesPerBeat(60.0, 48000.0), 48000.0),
           "bpmToSamplesPerBeat(60.0, 48000.0) → 48000.0", success);

    expect(nearEq(T::bpmToSamplesPerBeat(0.0, 44100.0), 0.0),
           "bpmToSamplesPerBeat(0.0, 44100.0) guard → 0.0", success);

    expect(nearEq(T::bpmToSamplesPerBeat(120.0, 0.0), 0.0),
           "bpmToSamplesPerBeat(120.0, 0.0) guard → 0.0", success);

    // ------------------------------------------------------------------ //
    // round-trip: beatsToSamples / samplesToBeats                         //
    // ------------------------------------------------------------------ //
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        for (double bpm : { 60.0, 120.0, 180.0 })
        {
            const double inputBeats  = 3.5;
            const int64_t samples    = T::beatsToSamples(inputBeats, bpm, sr);
            const double  roundTrip  = T::samplesToBeats(samples, bpm, sr);
            // Integer truncation in beatsToSamples may introduce up to 1 sample
            // of error; allow up to 1 sample worth of beat error.
            const double allowedErr  = 1.0 / T::bpmToSamplesPerBeat(bpm, sr) + kEps;
            expect(std::abs(roundTrip - inputBeats) < allowedErr,
                   "round-trip beatsToSamples/samplesToBeats failed", success);
        }
    }

    // ------------------------------------------------------------------ //
    // Transport state machine                                              //
    // ------------------------------------------------------------------ //
    {
        ssbb::Transport t;
        t.prepare(44100.0, 512);

        // Default tempo
        expect(nearEq(t.getTempo(), 120.0),
               "default tempo should be 120.0", success);

        // setTempo
        t.setTempo(120.0);
        expect(nearEq(t.getTempo(), 120.0),
               "getTempo() after setTempo(120.0) → 120.0", success);

        // play
        t.play();
        expect(t.isPlaying(), "isPlaying() after play() → true", success);

        // process advances position
        t.process(22050);
        expect(t.getPositionInSamples() == 22050,
               "getPositionInSamples() after process(22050) → 22050", success);

        // stop
        t.stop();
        expect(!t.isPlaying(), "isPlaying() after stop() → false", success);

        // process does NOT advance when stopped
        t.process(512);
        expect(t.getPositionInSamples() == 22050,
               "getPositionInSamples() unchanged while stopped", success);
    }

    // ------------------------------------------------------------------ //
    // Loop wrap                                                            //
    // ------------------------------------------------------------------ //
    {
        ssbb::Transport t;
        t.prepare(44100.0, 512);
        t.setTempo(120.0);

        // Loop range: 0–2 beats.  At 120 BPM / 44100 Hz that is 44100 samples.
        t.setLoopRange(0.0, 2.0);
        t.setLoopEnabled(true);
        t.play();

        // Process exactly one loop length from the current position (22050 from
        // the previous test scope's residual state is not here; fresh object above).
        // Position starts at 0; adding 44100 should wrap back to 0.
        t.process(44100);
        expect(t.getPositionInSamples() < 44100,
               "position should wrap below loopEnd (44100) after one loop", success);

        // Process a partial block that doesn't cross the loop boundary.
        const int64_t before = t.getPositionInSamples();
        t.process(512);
        const int64_t after  = t.getPositionInSamples();
        // After wrapping, we're somewhere in [0, 44100).  Adding 512 should keep
        // us below 44100 (true as long as before < 43588).
        if (before < (44100 - 512))
        {
            expect(after == before + 512,
                   "position should advance by 512 when not crossing loop boundary",
                   success);
        }
    }

    // ------------------------------------------------------------------ //
    // setTempo clamp                                                       //
    // ------------------------------------------------------------------ //
    {
        ssbb::Transport t;
        t.prepare(44100.0, 512);

        t.setTempo(9999.0);
        expect(nearEq(t.getTempo(), 300.0),
               "setTempo(9999.0) should clamp to 300.0", success);

        t.setTempo(-5.0);
        expect(nearEq(t.getTempo(), 20.0),
               "setTempo(-5.0) should clamp to 20.0", success);

        t.setTempo(120.0);
        expect(nearEq(t.getTempo(), 120.0),
               "setTempo(120.0) should stay at 120.0 (within range)", success);

        // Boundary values
        t.setTempo(20.0);
        expect(nearEq(t.getTempo(), 20.0),
               "setTempo(20.0) boundary — should be exactly 20.0", success);

        t.setTempo(300.0);
        expect(nearEq(t.getTempo(), 300.0),
               "setTempo(300.0) boundary — should be exactly 300.0", success);
    }

    // ------------------------------------------------------------------ //
    // Time signature setters                                               //
    // ------------------------------------------------------------------ //
    {
        ssbb::Transport t;
        t.prepare(44100.0, 512);

        t.setTimeSignature(3, 8);
        expect(t.getTimeSigNumerator()   == 3,
               "getTimeSigNumerator() after setTimeSignature(3,8) → 3", success);
        expect(t.getTimeSigDenominator() == 8,
               "getTimeSigDenominator() after setTimeSignature(3,8) → 8", success);
    }

    // ------------------------------------------------------------------ //
    // setPositionInBeats                                                   //
    // ------------------------------------------------------------------ //
    {
        ssbb::Transport t;
        t.prepare(44100.0, 512);
        t.setTempo(120.0);
        t.setPositionInBeats(1.0);
        // 1 beat at 120 BPM / 44100 Hz = 22050 samples
        expect(t.getPositionInSamples() == 22050,
               "setPositionInBeats(1.0) → 22050 samples at 120 BPM / 44100 Hz",
               success);
    }

    // ------------------------------------------------------------------ //
    // Result                                                               //
    // ------------------------------------------------------------------ //
    if (success)
        std::cout << "All transport tests passed.\n";
    else
        std::cerr << "One or more transport tests FAILED.\n";

    return success ? 0 : 1;
}
