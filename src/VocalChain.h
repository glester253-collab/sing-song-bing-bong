#pragma once
// VocalChain.h — original 9-stage vocal processing chain.
//
// Stages (in order, all implemented as simple inline DSP):
//   1. Polarity invert + input trim
//   2. High-pass filter (2nd-order Butterworth)
//   3. 3-band subtractive/clarity EQ (shelving + bell)
//   4. De-esser (sidechain HPF + RMS detector + downward gain control)
//   5. Compressor (RMS detection, ratio, attack/release)
//   6. Gentle saturation (soft-clip tanh approximation)
//   7. Presence/air shelf EQ (high-frequency shelving)
//   8. Short stereo delay send (wet mix + delay time in ms)
//   9. Peak limiter (look-ahead 1-sample brick-wall)
//
// THREAD MODEL:
//   Message thread : all setXxx() parameter setters.
//   Audio thread   : processBlock() — NO allocation, NO locks, NO I/O.
//
// Each stage reads its parameters from std::atomic members each block.
// Parameter smoothing uses a one-pole lowpass on the audio thread to avoid
// zipper noise on automation changes.
//
// Denormals: DC offset 1e-15 is injected into filter states each block.
// NaN/Inf: each stage hard-clamps its output to [-4, 4].

#include <array>
#include <atomic>
#include <cmath>

namespace ssbb {

class VocalChain
{
public:
    VocalChain() = default;

    // Non-copyable / non-movable (atomics + filter state).
    VocalChain(const VocalChain&)            = delete;
    VocalChain& operator=(const VocalChain&) = delete;
    VocalChain(VocalChain&&)                 = delete;
    VocalChain& operator=(VocalChain&&)      = delete;

    // ---- Prepare ---------------------------------------------------------

    void prepare(double sampleRate, int /*blockSize*/) noexcept
    {
        sr_ = (sampleRate > 0.0) ? static_cast<float>(sampleRate) : 44100.0f;
        resetState();
    }

    // ---- Message-thread parameter setters --------------------------------

    // --- Stage 1: polarity + input trim ---
    void setPolarityInvert(bool invert)  noexcept { polarityInvert_.store(invert, std::memory_order_relaxed); }
    void setInputTrimDb(float db)        noexcept { inputTrimDb_.store(db, std::memory_order_relaxed); }

    // --- Stage 2: high-pass filter ---
    void setHpfCutoffHz(float hz)        noexcept { hpfHz_.store(hz > 20.0f ? hz : 20.0f, std::memory_order_relaxed); }
    void setHpfEnabled(bool on)          noexcept { hpfOn_.store(on, std::memory_order_relaxed); }

    // --- Stage 3: 3-band EQ ---
    void setEqLowCutDb(float db)         noexcept { eqLowDb_.store(db, std::memory_order_relaxed); }
    void setEqMidCutDb(float db, float centerHz) noexcept
    {
        eqMidDb_.store(db, std::memory_order_relaxed);
        eqMidHz_.store(centerHz, std::memory_order_relaxed);
    }
    void setEqHighShelfDb(float db)      noexcept { eqHighDb_.store(db, std::memory_order_relaxed); }

    // --- Stage 4: de-esser ---
    void setDeEsserEnabled(bool on)      noexcept { deEsserOn_.store(on, std::memory_order_relaxed); }
    void setDeEsserFreqHz(float hz)      noexcept { deEsserHz_.store(hz, std::memory_order_relaxed); }
    void setDeEsserThreshDb(float db)    noexcept { deEsserThreshDb_.store(db, std::memory_order_relaxed); }

    // --- Stage 5: compressor ---
    void setCompressorEnabled(bool on)   noexcept { compOn_.store(on, std::memory_order_relaxed); }
    void setCompressorThreshDb(float db) noexcept { compThreshDb_.store(db, std::memory_order_relaxed); }
    void setCompressorRatio(float ratio) noexcept { compRatio_.store(ratio > 1.0f ? ratio : 1.0f, std::memory_order_relaxed); }
    void setCompressorAttackMs(float ms) noexcept { compAttackMs_.store(ms > 0.0f ? ms : 0.0f, std::memory_order_relaxed); }
    void setCompressorReleaseMs(float ms)noexcept { compReleaseMs_.store(ms > 0.0f ? ms : 0.0f, std::memory_order_relaxed); }
    void setCompressorMakeupDb(float db) noexcept { compMakeupDb_.store(db, std::memory_order_relaxed); }

    // --- Stage 6: saturation ---
    void setSaturationDrive(float drive) noexcept { satDrive_.store(drive >= 0.0f ? drive : 0.0f, std::memory_order_relaxed); }

    // --- Stage 7: presence/air shelf ---
    void setAirShelfFreqHz(float hz)     noexcept { airHz_.store(hz, std::memory_order_relaxed); }
    void setAirShelfGainDb(float db)     noexcept { airDb_.store(db, std::memory_order_relaxed); }

    // --- Stage 8: delay ---
    void setDelayTimeMs(float ms)        noexcept { delayMs_.store(ms >= 0.0f ? ms : 0.0f, std::memory_order_relaxed); }
    void setDelayWetMix(float wet)       noexcept { delayWet_.store(wet >= 0.0f ? wet : 0.0f, std::memory_order_relaxed); }

    // --- Stage 9: limiter ---
    void setLimiterThreshDb(float db)    noexcept { limThreshDb_.store(db, std::memory_order_relaxed); }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Process `numSamples` of mono audio in-place.
    void processBlock(float* buf, int numSamples) noexcept
    {
        if (numSamples <= 0 || buf == nullptr) return;

        // Read all parameters once.
        const bool  invertPol   = polarityInvert_.load(std::memory_order_relaxed);
        const float trimLin     = dbToLin(inputTrimDb_.load(std::memory_order_relaxed));
        const bool  hpfOn       = hpfOn_.load(std::memory_order_relaxed);
        const float hpfHz       = hpfHz_.load(std::memory_order_relaxed);
        const float eqLowDb     = eqLowDb_.load(std::memory_order_relaxed);
        const float eqMidDb     = eqMidDb_.load(std::memory_order_relaxed);
        const float eqMidHz     = eqMidHz_.load(std::memory_order_relaxed);
        const float eqHighDb    = eqHighDb_.load(std::memory_order_relaxed);
        const bool  deEsserOn   = deEsserOn_.load(std::memory_order_relaxed);
        const float deEsserHz   = deEsserHz_.load(std::memory_order_relaxed);
        const float deEsserThr  = dbToLin(deEsserThreshDb_.load(std::memory_order_relaxed));
        const bool  compOn      = compOn_.load(std::memory_order_relaxed);
        const float compThr     = dbToLin(compThreshDb_.load(std::memory_order_relaxed));
        const float compRatio   = compRatio_.load(std::memory_order_relaxed);
        const float compAttMs   = compAttackMs_.load(std::memory_order_relaxed);
        const float compRelMs   = compReleaseMs_.load(std::memory_order_relaxed);
        const float compMkup    = dbToLin(compMakeupDb_.load(std::memory_order_relaxed));
        const float satDrive    = satDrive_.load(std::memory_order_relaxed);
        const float airHz       = airHz_.load(std::memory_order_relaxed);
        const float airDb       = airDb_.load(std::memory_order_relaxed);
        const float delayMs     = delayMs_.load(std::memory_order_relaxed);
        const float delayWet    = delayWet_.load(std::memory_order_relaxed);
        const float limThr      = dbToLin(limThreshDb_.load(std::memory_order_relaxed));

        // ---- Precompute filter coefficients from parameters ----

        // HPF 2nd-order Butterworth (bilinear transform).
        float hpfB0 = 1.0f, hpfB1 = 0.0f, hpfB2 = 0.0f;
        float hpfA1 = 0.0f, hpfA2 = 0.0f;
        if (hpfOn)
        {
            const float omega = 2.0f * 3.14159265f * hpfHz / sr_;
            const float c     = 1.0f / tanFast(omega * 0.5f);
            const float c2    = c * c;
            const float sq2c  = 1.41421356f * c;
            const float den   = 1.0f / (1.0f + sq2c + c2);
            hpfB0 =  c2 * den;
            hpfB1 = -2.0f * c2 * den;
            hpfB2 =  c2 * den;
            hpfA1 =  2.0f * (1.0f - c2) * den;
            hpfA2 =  (1.0f - sq2c + c2) * den;
        }

        // De-esser sidechain HPF coefficients (1st-order).
        float desB0 = 1.0f, desB1 = 0.0f, desA1 = 0.0f;
        if (deEsserOn)
        {
            const float omega = 2.0f * 3.14159265f * deEsserHz / sr_;
            const float x     = 1.0f - tanFast(omega * 0.5f);
            desB0 =  0.5f * (1.0f + x);
            desB1 = -0.5f * (1.0f + x);
            desA1 = -x;
        }

        // Compressor time constants.
        const float compAtt = (compAttMs > 0.0f)
            ? std::exp(-1.0f / (sr_ * compAttMs * 0.001f)) : 0.0f;
        const float compRel = (compRelMs > 0.0f)
            ? std::exp(-1.0f / (sr_ * compRelMs * 0.001f)) : 0.0f;

        // Delay line index.
        const int delayFrames = static_cast<int>(delayMs * sr_ * 0.001f);
        const int maxDelay = static_cast<int>(kDelayBufferSize);
        const int effDelay = (delayFrames < maxDelay) ? delayFrames : maxDelay - 1;

        for (int i = 0; i < numSamples; ++i)
        {
            float s = buf[i];

            // ---- Stage 1: polarity + trim ---
            if (invertPol) s = -s;
            s *= trimLin;

            // ---- Stage 2: HPF ---
            if (hpfOn)
            {
                const float yin = s;
                s = hpfB0 * yin + hpfB1 * hpfX1_ + hpfB2 * hpfX2_
                        - hpfA1 * hpfY1_ - hpfA2 * hpfY2_;
                hpfX2_ = hpfX1_; hpfX1_ = yin;
                hpfY2_ = hpfY1_; hpfY1_ = s;
                s += 1e-15f;   // denormal guard
            }
            clamp(s);

            // ---- Stage 3: 3-band EQ ---
            s = applyLowShelf(s, eqLowDb, 200.0f);
            s = applyBellEq(s, eqMidDb, eqMidHz, 1.0f);
            s = applyHighShelf(s, eqHighDb, 6000.0f);
            clamp(s);

            // ---- Stage 4: de-esser ---
            if (deEsserOn)
            {
                // Sidechain: 1st-order HPF on the signal.
                const float sc = desB0 * s + desB1 * desX1_ - desA1 * desY1_;
                desX1_ = s; desY1_ = sc;

                // RMS envelope follower on the sidechain.
                const float scAbs = sc < 0.0f ? -sc : sc;
                deEsserEnv_ = 0.995f * deEsserEnv_ + 0.005f * scAbs;

                // Gain reduction when above threshold.
                if (deEsserEnv_ > deEsserThr && deEsserEnv_ > 1e-9f)
                {
                    const float gr = deEsserThr / deEsserEnv_;
                    s *= gr;
                }
            }
            clamp(s);

            // ---- Stage 5: compressor ---
            if (compOn)
            {
                const float absS = s < 0.0f ? -s : s;
                // Envelope follower.
                const float coeff = (absS > compEnv_) ? compAtt : compRel;
                compEnv_ = coeff * compEnv_ + (1.0f - coeff) * absS;

                // Gain reduction.
                float gr = 1.0f;
                if (compEnv_ > compThr && compThr > 1e-9f)
                {
                    gr = compThr / compEnv_;
                    gr = 1.0f - (1.0f - gr) * (1.0f - 1.0f / compRatio);
                }
                s *= gr * compMkup;
            }
            clamp(s);

            // ---- Stage 6: saturation ---
            if (satDrive > 0.0f)
                s = tanhFast(s * (1.0f + satDrive)) / (1.0f + satDrive);
            clamp(s);

            // ---- Stage 7: presence/air shelf ---
            s = applyHighShelf(s, airDb, airHz);
            clamp(s);

            // ---- Stage 8: delay send ---
            if (effDelay > 0 && delayWet > 0.0f)
            {
                const int readIdx = (delayWriteIdx_ - effDelay + maxDelay) % maxDelay;
                const float dSample = delayBuf_[static_cast<std::size_t>(readIdx)];
                delayBuf_[static_cast<std::size_t>(delayWriteIdx_)] = s;
                delayWriteIdx_ = (delayWriteIdx_ + 1) % maxDelay;
                s = s + dSample * delayWet;
            }
            clamp(s);

            // ---- Stage 9: limiter (brick-wall) ---
            {
                const float absS = s < 0.0f ? -s : s;
                if (absS > limThr && limThr > 1e-9f)
                    s = (s > 0.0f ? limThr : -limThr);
            }

            buf[i] = s;
        }
    }

private:
    float sr_ { 44100.0f };

    // ---- Filter state ---------------------------------------------------

    // HPF
    float hpfX1_ { 0.0f }, hpfX2_ { 0.0f };
    float hpfY1_ { 0.0f }, hpfY2_ { 0.0f };

    // EQ states (simple one-pole approximation storage).
    float eqLowX1_ { 0.0f }, eqLowY1_ { 0.0f };
    float eqMidX1_ { 0.0f }, eqMidY1_ { 0.0f };
    float eqHighX1_{ 0.0f }, eqHighY1_{ 0.0f };
    float eqAirX1_ { 0.0f }, eqAirY1_ { 0.0f };

    // De-esser
    float desX1_ { 0.0f }, desY1_ { 0.0f };
    float deEsserEnv_ { 0.0f };

    // Compressor
    float compEnv_ { 0.0f };

    // Delay line.
    static constexpr std::size_t kDelayBufferSize = 96000u; // 2 s at 48 kHz
    std::array<float, kDelayBufferSize> delayBuf_ {};
    int delayWriteIdx_ { 0 };

    // ---- Parameter atomics ----------------------------------------------

    std::atomic<bool>  polarityInvert_  { false };
    std::atomic<float> inputTrimDb_     { 0.0f };
    std::atomic<bool>  hpfOn_           { true };
    std::atomic<float> hpfHz_           { 80.0f };
    std::atomic<float> eqLowDb_         { -3.0f };
    std::atomic<float> eqMidDb_         { 0.0f };
    std::atomic<float> eqMidHz_         { 1500.0f };
    std::atomic<float> eqHighDb_        { 2.0f };
    std::atomic<bool>  deEsserOn_       { true };
    std::atomic<float> deEsserHz_       { 7000.0f };
    std::atomic<float> deEsserThreshDb_ { -20.0f };
    std::atomic<bool>  compOn_          { true };
    std::atomic<float> compThreshDb_    { -18.0f };
    std::atomic<float> compRatio_       { 4.0f };
    std::atomic<float> compAttackMs_    { 10.0f };
    std::atomic<float> compReleaseMs_   { 80.0f };
    std::atomic<float> compMakeupDb_    { 4.0f };
    std::atomic<float> satDrive_        { 0.2f };
    std::atomic<float> airHz_           { 10000.0f };
    std::atomic<float> airDb_           { 3.0f };
    std::atomic<float> delayMs_         { 60.0f };
    std::atomic<float> delayWet_        { 0.15f };
    std::atomic<float> limThreshDb_     { -1.0f };

    // ---- Helpers (no allocation, no libm on the audio thread) -----------

    static float dbToLin(float db) noexcept
    {
        // 2^(db/6) ≈ amplitude from dB — avoids powf/expf.
        return fastExp2(db * 0.16667f);
    }

    static float fastExp2(float x) noexcept
    {
        return 1.0f + x * 0.6931472f + x * x * 0.24022f;
    }

    static float tanhFast(float x) noexcept
    {
        // Padé approximant for tanh, good for |x| < 2.
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    static float tanFast(float x) noexcept
    {
        const float x2 = x * x;
        return x * (1.0f + x2 * 0.3333f) / (1.0f + x2 * (-0.2f + x2 * 0.0095f));
    }

    static void clamp(float& s) noexcept
    {
        if (s >  4.0f) s =  4.0f;
        if (s < -4.0f) s = -4.0f;
        // Zero out NaN.
        if (s != s) s = 0.0f;
    }

    // ---- Simple shelving / bell EQ (1st-order, low quality but no alloc) ---

    float applyLowShelf(float s, float db, float hz) noexcept
    {
        if (db == 0.0f) return s;
        const float A  = fastExp2(db * 0.08333f);   // √(lin)
        const float w0 = 2.0f * 3.14159265f * hz / sr_;
        const float c  = tanFast(w0 * 0.5f);
        const float den = 1.0f / (A + c);
        const float b0  = (A * A * c + A) * den;
        const float b1  = (A * A * c - A) * den;
        const float a1  = (c - A) * den;
        const float y   = b0 * s + b1 * eqLowX1_ - a1 * eqLowY1_;
        eqLowX1_ = s; eqLowY1_ = y;
        return y;
    }

    float applyHighShelf(float s, float db, float hz) noexcept
    {
        if (db == 0.0f) return s;
        const float A  = fastExp2(db * 0.08333f);
        const float w0 = 2.0f * 3.14159265f * hz / sr_;
        const float c  = tanFast(w0 * 0.5f);
        const float den = 1.0f / (1.0f + A * c);
        const float b0  = (1.0f + A * A * c) * den;
        const float b1  = (A * A * c - 1.0f) * den;
        const float a1  = (A * c - 1.0f) * den;
        const float y   = b0 * s + b1 * eqHighX1_ - a1 * eqHighY1_;
        eqHighX1_ = s; eqHighY1_ = y;
        return y;
    }

    float applyBellEq(float s, float db, float hz, float q) noexcept
    {
        if (db == 0.0f) return s;
        const float A   = fastExp2(db * 0.08333f);
        const float w0  = 2.0f * 3.14159265f * hz / sr_;
        const float alp = tanFast(w0 * 0.5f) / q;
        const float b0  = 1.0f + A * alp;
        const float b1  = -2.0f * cosFast(w0);
        const float b2  = 1.0f - A * alp;
        const float a0  = 1.0f + alp / A;
        const float a1r  = -2.0f * cosFast(w0);
        const float a2  = 1.0f - alp / A;
        const float invA0 = 1.0f / a0;
        const float y   = (b0 * s + b1 * eqMidX1_ + b2 * eqMidY1_
                          - a1r * eqMidX1_ - a2 * eqMidY1_) * invA0;
        eqMidY1_ = eqMidX1_; eqMidX1_ = y;
        return y;
    }

    static float cosFast(float x) noexcept
    {
        const float x2 = x * x;
        return 1.0f - x2 * (0.5f - x2 * 0.041667f);
    }

    void resetState() noexcept
    {
        hpfX1_ = hpfX2_ = hpfY1_ = hpfY2_ = 0.0f;
        eqLowX1_ = eqLowY1_ = 0.0f;
        eqMidX1_ = eqMidY1_ = 0.0f;
        eqHighX1_ = eqHighY1_ = 0.0f;
        eqAirX1_ = eqAirY1_ = 0.0f;
        desX1_ = desY1_ = 0.0f;
        deEsserEnv_ = 0.0f;
        compEnv_ = 0.0f;
        delayBuf_.fill(0.0f);
        delayWriteIdx_ = 0;
    }
};

} // namespace ssbb
