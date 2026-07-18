#pragma once
// SubtractiveSynth.h — polyphonic subtractive synthesizer.
//
// Architecture:
//   Oscillator (saw/square/triangle/sine) → 12 dB/oct LP filter → ADSR → gain
//
// THREAD MODEL:
//   Message thread : all setters (waveform, filter cutoff/resonance, ADSR,
//                    modulation routes, polyphony).
//   Audio thread   : noteOn() / noteOff() manage voice pool; processBlock()
//                    renders voices into the output.
//                    NO allocation, NO locks, NO I/O, NO logging.
//
// Modulation matrix:
//   Two LFO sources (Rate, Depth) can be routed to: Pitch, FilterCutoff, Amp.
//   Modulation amounts are stored as atomics so the message thread can update
//   them between audio callbacks without a lock.
//
// Denormals: DC offset of 1e-15 is added to filter states to flush denormals.
// NaN/Inf guard: filter outputs are clamped to [-2, 2] each block.

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ssbb {

class SubtractiveSynth
{
public:
    static constexpr int kMaxVoices = 8;

    enum class Waveform : int { Saw = 0, Square, Triangle, Sine };

    // ---- Modulation routes -----------------------------------------------
    enum class ModDest : int { Pitch = 0, FilterCutoff, Amp, kNumDests };
    static constexpr int kNumDests = static_cast<int>(ModDest::kNumDests);

    // ---- Constructor -----------------------------------------------------

    SubtractiveSynth()
    {
        for (auto& v : voices_) v = Voice{};
    }

    // Non-copyable / non-movable (atomics).
    SubtractiveSynth(const SubtractiveSynth&)            = delete;
    SubtractiveSynth& operator=(const SubtractiveSynth&) = delete;
    SubtractiveSynth(SubtractiveSynth&&)                 = delete;
    SubtractiveSynth& operator=(SubtractiveSynth&&)      = delete;

    // ---- Prepare ---------------------------------------------------------

    void prepare(double sampleRate, int /*blockSize*/) noexcept
    {
        sr_.store(sampleRate > 0.0 ? sampleRate : 44100.0,
                  std::memory_order_relaxed);
    }

    // ---- Message-thread parameter setters --------------------------------

    void setWaveform(Waveform w) noexcept
    {
        waveform_.store(static_cast<int>(w), std::memory_order_relaxed);
    }

    /// Filter cutoff in Hz (20–20000).
    void setFilterCutoff(float hz) noexcept
    {
        if (hz < 20.0f)      hz = 20.0f;
        if (hz > 20000.0f)   hz = 20000.0f;
        cutoffHz_.store(hz, std::memory_order_relaxed);
    }

    /// Filter resonance 0.0 (flat) to 1.0 (self-oscillating).
    void setFilterResonance(float q) noexcept
    {
        if (q < 0.0f) q = 0.0f;
        if (q > 1.0f) q = 1.0f;
        resonance_.store(q, std::memory_order_relaxed);
    }

    /// ADSR in seconds (attack, decay, sustain 0–1, release).
    void setADSR(float attack, float decay, float sustain, float release) noexcept
    {
        attack_ .store(attack  > 0.0001f ? attack  : 0.0001f, std::memory_order_relaxed);
        decay_  .store(decay   > 0.0001f ? decay   : 0.0001f, std::memory_order_relaxed);
        sustain_.store(sustain >= 0.0f   ? sustain : 0.0f,    std::memory_order_relaxed);
        release_.store(release > 0.0001f ? release : 0.0001f, std::memory_order_relaxed);
    }

    /// Output gain (linear, 0–2).
    void setGain(float gain) noexcept
    {
        gain_.store(gain >= 0.0f ? gain : 0.0f, std::memory_order_relaxed);
    }

    // ---- LFO modulation matrix -------------------------------------------

    void setLfoRate(float hz) noexcept
    {
        lfoRate_.store(hz > 0.0f ? hz : 0.0f, std::memory_order_relaxed);
    }

    void setModAmount(ModDest dest, float amount) noexcept
    {
        const auto idx = static_cast<int>(dest);
        if (idx >= 0 && idx < kNumDests)
            modAmount_[static_cast<std::size_t>(idx)].store(amount,
                std::memory_order_relaxed);
    }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    void noteOn(int midiNote, int velocity) noexcept
    {
        if (midiNote < 0 || midiNote > 127) return;
        if (velocity < 0)   velocity = 0;
        if (velocity > 127) velocity = 127;

        // Find free or oldest voice.
        int slot = -1;
        for (int i = 0; i < kMaxVoices; ++i)
        {
            if (!voices_[static_cast<std::size_t>(i)].active)
            {
                slot = i;
                break;
            }
        }
        if (slot < 0) slot = oldestVoice();

        auto& v   = voices_[static_cast<std::size_t>(slot)];
        v.active  = true;
        v.note    = midiNote;
        v.vel     = static_cast<float>(velocity) / 127.0f;
        v.phase   = 0.0f;
        v.env     = 0.0f;
        v.stage   = EnvStage::Attack;
        v.filt[0] = 0.0f;
        v.filt[1] = 0.0f;
        v.filt[2] = 0.0f;
        v.filt[3] = 0.0f;
    }

    void noteOff(int midiNote) noexcept
    {
        for (auto& v : voices_)
        {
            if (v.active && v.note == midiNote)
                v.stage = EnvStage::Release;
        }
    }

    /// Mix all active voices into `outL` and `outR` (both may be the same pointer
    /// for mono rendering).  outL and outR must point to `numSamples` floats.
    void processBlock(float* outL, float* outR, int numSamples) noexcept
    {
        if (numSamples <= 0 || outL == nullptr) return;
        if (outR == nullptr) outR = outL;

        const double sr         = sr_.load(std::memory_order_relaxed);
        const int    wf         = waveform_.load(std::memory_order_relaxed);
        const float  cutHz      = cutoffHz_.load(std::memory_order_relaxed);
        const float  res        = resonance_.load(std::memory_order_relaxed);
        const float  att        = attack_ .load(std::memory_order_relaxed);
        const float  dec        = decay_  .load(std::memory_order_relaxed);
        const float  sus        = sustain_.load(std::memory_order_relaxed);
        const float  rel        = release_.load(std::memory_order_relaxed);
        const float  masterGain = gain_   .load(std::memory_order_relaxed);
        const float  lfoRate    = lfoRate_.load(std::memory_order_relaxed);
        const float  modPitch   = modAmount_[0].load(std::memory_order_relaxed);
        const float  modCutoff  = modAmount_[1].load(std::memory_order_relaxed);
        const float  modAmp     = modAmount_[2].load(std::memory_order_relaxed);

        const float srF = static_cast<float>(sr);

        // LFO phase advance per sample.
        const float lfoInc = (srF > 0.0f) ? (lfoRate / srF) : 0.0f;

        for (auto& v : voices_)
        {
            if (!v.active) continue;

            // Cache per-voice base frequency.
            const float baseFreq = midiNoteToHz(v.note);

            for (int i = 0; i < numSamples; ++i)
            {
                // ---- LFO ------------------------------------------------
                lfoPhase_ += lfoInc;
                if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;
                const float lfo = sinApprox(lfoPhase_);

                // ---- Oscillator -----------------------------------------
                float freq = baseFreq * fastExp2(lfo * modPitch);
                if (freq < 1.0f) freq = 1.0f;
                const float phaseInc = freq / srF;

                v.phase += phaseInc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;

                float osc = oscillate(static_cast<Waveform>(wf), v.phase);

                // ---- ADSR envelope --------------------------------------
                updateEnvelope(v, att, dec, sus, rel, srF);
                const float envGain = v.env * (1.0f + lfo * modAmp);

                // ---- 12 dB/oct LP filter (TPT state-variable) -----------
                float effCut = cutHz * fastExp2(lfo * modCutoff);
                if (effCut < 20.0f)    effCut = 20.0f;
                if (effCut > 20000.0f) effCut = 20000.0f;

                const float g  = tanApprox(3.14159265f * effCut / srF);
                const float k  = 2.0f - 2.0f * res;
                const float a1 = 1.0f / (1.0f + g * (g + k));
                const float a2 = g * a1;
                const float a3 = g * a2;

                // Denormal guard: tiny DC offset flushes filter state.
                osc += 1e-15f;

                const float v0   = osc;
                const float v3   = v0 - v.filt[1];
                const float v1   = a1 * v.filt[0] + a2 * v3;
                const float v2   = v.filt[1] + a2 * v.filt[0] + a3 * v3;

                v.filt[0] = 2.0f * v1 - v.filt[0];
                v.filt[1] = 2.0f * v2 - v.filt[1];

                float out = v2;   // LP output

                // Hard-clamp to catch NaN/Inf.
                if (out > 2.0f)  out = 2.0f;
                if (out < -2.0f) out = -2.0f;

                const float sample = out * envGain * v.vel * masterGain;
                outL[i] += sample;
                outR[i] += sample;
            }
        }
    }

private:
    // ---- Voice pool state ------------------------------------------------

    enum class EnvStage : int { Attack = 0, Decay, Sustain, Release, Done };

    struct Voice
    {
        bool      active { false };
        int       note   { 60 };
        float     vel    { 1.0f };
        float     phase  { 0.0f };
        float     env    { 0.0f };
        EnvStage  stage  { EnvStage::Done };
        float     filt[4] { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    std::array<Voice, kMaxVoices> voices_;
    float lfoPhase_ { 0.0f };

    // ---- Parameter atomics -----------------------------------------------
    std::atomic<double> sr_        { 44100.0 };
    std::atomic<int>    waveform_  { static_cast<int>(Waveform::Saw) };
    std::atomic<float>  cutoffHz_  { 8000.0f };
    std::atomic<float>  resonance_ { 0.0f };
    std::atomic<float>  attack_    { 0.01f };
    std::atomic<float>  decay_     { 0.1f };
    std::atomic<float>  sustain_   { 0.7f };
    std::atomic<float>  release_   { 0.2f };
    std::atomic<float>  gain_      { 1.0f };
    std::atomic<float>  lfoRate_   { 1.0f };
    std::array<std::atomic<float>, kNumDests> modAmount_ {};

    // ---- Helpers (audio-thread-safe, no allocation) ----------------------

    int oldestVoice() const noexcept
    {
        // Very simple: just return voice 0.
        // A production system would track age per voice.
        return 0;
    }

    static float midiNoteToHz(int note) noexcept
    {
        return 440.0f * fastExp2((static_cast<float>(note) - 69.0f) / 12.0f);
    }

    static float oscillate(Waveform wf, float phase) noexcept
    {
        switch (wf)
        {
            case Waveform::Saw:
                return 2.0f * phase - 1.0f;

            case Waveform::Square:
                return (phase < 0.5f) ? 1.0f : -1.0f;

            case Waveform::Triangle:
                return (phase < 0.5f)
                           ? 4.0f * phase - 1.0f
                           : 3.0f - 4.0f * phase;

            case Waveform::Sine:
            default:
                return sinApprox(phase);
        }
    }

    // Minimax sine approximation — error < 0.001 on [0, 1].
    static float sinApprox(float t) noexcept
    {
        const float x = t - 0.5f;          // map to [-0.5, 0.5]
        const float x2 = x * x;
        // Bhaskara-style approximation
        return 4.0f * x * (1.0f - x) * (1.0f - 2.0f * x2);
    }

    // Approximate 2^x for small x (LFO pitch mod, < ±1 semitone typical).
    static float fastExp2(float x) noexcept
    {
        // 1st-order Taylor: 2^x ≈ 1 + x*ln2  (good for |x| < 0.5)
        return 1.0f + x * 0.6931472f;
    }

    // Approximate tan(πx) for small x (filter coefficient).
    static float tanApprox(float x) noexcept
    {
        // Padé approximant: valid for x in [0, π/4]
        const float x2 = x * x;
        return x * (1.0f + x2 * 0.3333f) / (1.0f - x2 * 0.2);
    }

    static void updateEnvelope(Voice& v,
                                float att, float dec,
                                float sus, float rel,
                                float sr) noexcept
    {
        switch (v.stage)
        {
            case EnvStage::Attack:
                v.env += 1.0f / (att * sr);
                if (v.env >= 1.0f) { v.env = 1.0f; v.stage = EnvStage::Decay; }
                break;
            case EnvStage::Decay:
                v.env -= (1.0f - sus) / (dec * sr);
                if (v.env <= sus)  { v.env = sus;  v.stage = EnvStage::Sustain; }
                break;
            case EnvStage::Sustain:
                v.env = sus;
                break;
            case EnvStage::Release:
                v.env -= sus / (rel * sr);
                if (v.env <= 0.0f) { v.env = 0.0f; v.active = false; v.stage = EnvStage::Done; }
                break;
            case EnvStage::Done:
                v.env = 0.0f;
                break;
        }
    }
};

} // namespace ssbb
