// transport_tests.cpp
// Plain C++20 — NO JUCE headers, NO third-party test frameworks.
// Links Transport.cpp and Metronome.cpp so Metronome timing can be tested
// without any JUCE dependency.
// Follow the same expect() pattern established in bootstrap_tests.cpp.
#include "Transport.h"       // ssbb::Transport — includes only <atomic> and <cstdint>
#include "Metronome.h"       // ssbb::Metronome — includes only <atomic> and <cstdint>

#include <cmath>             // std::abs, std::floor
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

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
    // Metronome: helpers                                                   //
    // ------------------------------------------------------------------ //
    // Returns true if any sample in buf within [center-tol, center+tol] is
    // non-zero.  The tolerance of 1 covers the sin(0)=0 behaviour: the sine
    // oscillator starts at phase 0 so the first output sample of each click
    // burst is 0; the first audible (non-zero) sample is at beat_position + 1.
    auto hasClickNear = [](const std::vector<float>& buf, int center, int tol = 1) -> bool
    {
        const int lo = std::max(0, center - tol);
        const int hi = std::min(static_cast<int>(buf.size()) - 1, center + tol);
        for (int i = lo; i <= hi; ++i)
            if (buf[i] != 0.0f) return true;
        return false;
    };

    // Returns true if the region [lo, hi) (exclusive hi) is entirely silent.
    auto isSilent = [](const std::vector<float>& buf, int lo, int hi) -> bool
    {
        for (int i = lo; i < hi; ++i)
            if (buf[i] != 0.0f) return false;
        return true;
    };

    // ------------------------------------------------------------------ //
    // Metronome: 4/4 click placement                                       //
    // ------------------------------------------------------------------ //
    {
        // 120 BPM, 44100 Hz, 4/4 → quarter note = 22050 samples.
        // Process 2 beats (44100 samples) from position 0.
        // Expect clicks near samples 0 (accent, beat 0) and 22050 (beat 1);
        // mid-beat region should be silent.
        constexpr double kSR  = 44100.0;
        constexpr double kBPM = 120.0;
        constexpr int    kBuf = 44100;

        ssbb::Transport t44;
        t44.prepare(kSR, kBuf);
        t44.setTempo(kBPM);
        t44.setTimeSignature(4, 4);
        t44.play();

        ssbb::Metronome m44;
        m44.prepare(kSR, kBuf);
        m44.setEnabled(true);

        std::vector<float> buf44(kBuf, 0.0f);
        const int64_t bs44 = t44.getPositionInSamples(); // 0
        t44.process(kBuf);
        m44.processBlock(buf44.data(), kBuf, t44, bs44);

        expect(hasClickNear(buf44, 0),
               "4/4: click near sample 0 (beat 0)", success);
        expect(hasClickNear(buf44, 22050),
               "4/4: click near sample 22050 (beat 1)", success);
        // 10 ms click at 44100 Hz = 441 samples; mid-beat (e.g. sample 11000) is silent.
        expect(isSilent(buf44, 500, 22000),
               "4/4: silence in mid-beat region [500, 22000)", success);
    }

    // ------------------------------------------------------------------ //
    // Metronome: 3/4 click placement                                       //
    // ------------------------------------------------------------------ //
    {
        // 120 BPM, 44100 Hz, 3/4 → quarter note = 22050 samples.
        // Process 3 beats (66150 samples) from position 0.
        // Expect clicks near 0, 22050, 44100.
        constexpr double kSR  = 44100.0;
        constexpr double kBPM = 120.0;
        constexpr int    kBuf = 66150;

        ssbb::Transport t34;
        t34.prepare(kSR, kBuf);
        t34.setTempo(kBPM);
        t34.setTimeSignature(3, 4);
        t34.play();

        ssbb::Metronome m34;
        m34.prepare(kSR, kBuf);
        m34.setEnabled(true);

        std::vector<float> buf34(kBuf, 0.0f);
        const int64_t bs34 = t34.getPositionInSamples();
        t34.process(kBuf);
        m34.processBlock(buf34.data(), kBuf, t34, bs34);

        expect(hasClickNear(buf34, 0),
               "3/4: click near sample 0", success);
        expect(hasClickNear(buf34, 22050),
               "3/4: click near sample 22050 (beat 1)", success);
        expect(hasClickNear(buf34, 44100),
               "3/4: click near sample 44100 (beat 2)", success);
        expect(isSilent(buf34, 500, 22000),
               "3/4: silence in mid-beat region", success);
    }

    // ------------------------------------------------------------------ //
    // Metronome: 6/8 click placement                                       //
    // ------------------------------------------------------------------ //
    {
        // 120 BPM, 44100 Hz, 6/8.
        // samplesPerClick = 22050 * 4 / 8 = 11025 (one eighth note).
        // Process 6 eighth notes (66150 samples) from position 0.
        // Expect clicks near 0, 11025, 22050, 33075, 44100, 55125.
        // Mid-beat silence: [500, 10500).
        constexpr double kSR  = 44100.0;
        constexpr double kBPM = 120.0;
        constexpr int    kBuf = 66150;

        ssbb::Transport t68;
        t68.prepare(kSR, kBuf);
        t68.setTempo(kBPM);
        t68.setTimeSignature(6, 8);
        t68.play();

        ssbb::Metronome m68;
        m68.prepare(kSR, kBuf);
        m68.setEnabled(true);

        std::vector<float> buf68(kBuf, 0.0f);
        const int64_t bs68 = t68.getPositionInSamples();
        t68.process(kBuf);
        m68.processBlock(buf68.data(), kBuf, t68, bs68);

        expect(hasClickNear(buf68, 0),
               "6/8: click near sample 0 (1st eighth note)", success);
        expect(hasClickNear(buf68, 11025),
               "6/8: click near sample 11025 (2nd eighth note)", success);
        expect(hasClickNear(buf68, 22050),
               "6/8: click near sample 22050 (3rd eighth note)", success);
        expect(hasClickNear(buf68, 55125),
               "6/8: click near sample 55125 (6th eighth note)", success);
        // Mid-beat silence between first and second eighth note.
        expect(isSilent(buf68, 500, 10500),
               "6/8: silence in mid-eighth-note region [500, 10500)", success);
    }

    // ------------------------------------------------------------------ //
    // Metronome: click at loop boundary                                    //
    // ------------------------------------------------------------------ //
    {
        // 120 BPM, 44100 Hz, 4/4, loop 0..2 beats (0..44100 samples).
        // Position the transport near the loop end and process a block that
        // straddles the boundary so loop wrap occurs mid-block.
        //
        // blockStart = 44000, numSamples = 200:
        //   pre-wrap segment: samples [44000..44099] → block offsets 0..99
        //   wrap offset: 44100 - 44000 = 100
        //   post-wrap segment: loopStart + [0..99] = [0..99] → block offsets 100..199
        //
        // Beat 0 starts at timeline position 0 (loopStart).
        // Expected click onset inside this block: at block offset 100 (first
        // post-wrap sample, timeline position 0 = beat 0 = accent).
        constexpr double kSR      = 44100.0;
        constexpr double kBPM     = 120.0;
        constexpr int    kBuf     = 200;
        constexpr int64_t kBStart = 44000;

        ssbb::Transport tloop;
        tloop.prepare(kSR, kBuf);
        tloop.setTempo(kBPM);
        tloop.setTimeSignature(4, 4);
        tloop.setLoopRange(0.0, 2.0);   // 0..2 beats = 0..44100 samples
        tloop.setLoopEnabled(true);
        tloop.setPositionInBeats(0.0);  // start at 0

        // Manually advance position to blockStart without going through process()
        // (we must set it explicitly so we can pick an arbitrary starting point).
        // Reuse setPositionInBeats with a beats value that maps to 44000 samples:
        // beats = 44000 / 44100 * 120 / 60 ≈ 1.9955… — use direct sample math.
        // Since setPositionInBeats truncates, advance via multiple process() calls
        // until we overshoot, then reset. Simpler: just call prepare() to reset and
        // call process() in one big block up to kBStart while stopped, then play().
        // Easiest: set up position manually using the block-start offset.
        //
        // Actually we can set the position by calling setPositionInBeats with the
        // equivalent beat value. At 120 BPM / 44100 Hz: 1 beat = 22050 samples.
        // 44000 / 22050 ≈ 1.99546… beats.  samplesToBeats gives the exact value.
        tloop.setPositionInBeats(
            ssbb::Transport::samplesToBeats(kBStart, kBPM, kSR));
        tloop.play();

        // Verify the position landed at kBStart (within ±1 sample of integer truncation).
        const int64_t actualStart = tloop.getPositionInSamples();
        expect(std::abs(static_cast<long long>(actualStart - kBStart)) <= 1LL,
               "loop-boundary test: setPositionInBeats landed near kBStart", success);

        ssbb::Metronome mloop;
        mloop.prepare(kSR, kBuf);
        mloop.setEnabled(true);

        std::vector<float> bufloop(kBuf, 0.0f);
        const int64_t bsLoop = tloop.getPositionInSamples();
        tloop.process(kBuf);
        mloop.processBlock(bufloop.data(), kBuf, tloop, bsLoop);

        // The expected wrap offset within this block.
        const int expectedWrapOffset = static_cast<int>(44100LL - bsLoop);

        // A click onset should occur at the wrap offset (timeline pos 0 = beat 0).
        // Allow ±1 sample for integer rounding in beatsToSamples and the
        // sin(0)=0 first-sample behaviour of the click oscillator.
        expect(hasClickNear(bufloop, expectedWrapOffset, 2),
               "loop-boundary: click onset at loop wrap point (beat 0 accent)", success);
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
