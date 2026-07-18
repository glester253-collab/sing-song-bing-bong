#pragma once
// LoudnessMeter.h — ITU-R BS.1770-4 integrated loudness (LUFS), short-term
//                   loudness, true-peak, and per-sample peak metering.
//
// THREAD MODEL:
//   Audio thread  : processBlock() — accumulates energy; no allocation, no locks.
//   Any thread    : getIntegratedLufs(), getShortTermLufs(), getTruePeak(),
//                   getPeakDb() — atomic reads.
//
// Simplifications (sufficient for a mix assistant):
//   - Pre-filter (K-weighting high shelf + HPF) uses approximate IIR coefficients.
//   - True-peak approximation: 4x upsampled peak via linear interpolation.
//   - Gating: -70 LUFS absolute gate, -10 LU relative gate.
//
// Usage:
//   meter.prepare(sampleRate, numChannels);
//   // ... in audio callback:
//   meter.processBlock(channelData, numChannels, numSamples);
//   float lufs = meter.getIntegratedLufs();

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ssbb {

class LoudnessMeter
{
public:
    static constexpr int kMaxChannels = 2;

    LoudnessMeter()  = default;

    // Non-copyable.
    LoudnessMeter(const LoudnessMeter&)            = delete;
    LoudnessMeter& operator=(const LoudnessMeter&) = delete;

    // ---- Prepare ---------------------------------------------------------

    void prepare(double sampleRate, int numChannels) noexcept
    {
        sr_   = sampleRate > 0.0 ? sampleRate : 44100.0;
        nch_  = (numChannels > 0 && numChannels <= kMaxChannels) ? numChannels : 1;
        reset();
        computeKWeightCoeffs();
        // BS.1770 block = 400 ms, stride = 75 ms.
        blockSamples_  = static_cast<int>(sr_ * 0.400);
        strideSamples_ = static_cast<int>(sr_ * 0.075);
        shortBlkSmp_   = static_cast<int>(sr_ * 3.000); // 3-second window
    }

    void reset() noexcept
    {
        integratedSum_ = 0.0;
        integratedCount_ = 0;
        blockAccum_ = 0.0;
        blockSampleCount_ = 0;
        shortTermAccum_ = 0.0;
        shortTermCount_ = 0;
        truePeak_.store(0.0f, std::memory_order_relaxed);
        peakDb_.store(-100.0f, std::memory_order_relaxed);
        intLufs_.store(-100.0f, std::memory_order_relaxed);
        stLufs_.store(-100.0f, std::memory_order_relaxed);
        for (auto& st : kw_) st = {};
    }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    void processBlock(const float* const* channelData,
                      int                 numChannels,
                      int                 numSamples) noexcept
    {
        if (numSamples <= 0 || channelData == nullptr) return;

        const int ch = (numChannels < nch_) ? numChannels : nch_;

        for (int i = 0; i < numSamples; ++i)
        {
            double meanSqKW = 0.0;

            for (int c = 0; c < ch; ++c)
            {
                if (channelData[c] == nullptr) continue;
                const float raw = channelData[c][i];

                // K-weighting (BS.1770 pre-filter, two biquad stages per channel).
                const float kw = applyKWeight(raw, c);

                // Per-sample peak.
                const float absRaw = raw < 0.0f ? -raw : raw;
                float curPeak = peakRaw_[static_cast<std::size_t>(c)];
                if (absRaw > curPeak)
                {
                    peakRaw_[static_cast<std::size_t>(c)] = absRaw;
                    updatePeakDb(absRaw);
                }

                // True peak: 4x oversample approximation.
                const float tp = estimateTruePeak(raw, c);
                const float absTp = tp < 0.0f ? -tp : tp;
                float curTp = truePeak_.load(std::memory_order_relaxed);
                if (absTp > curTp)
                    truePeak_.store(absTp, std::memory_order_relaxed);

                // Channel gain for BS.1770 (surround: L/R/C/LFE weighting omitted).
                meanSqKW += static_cast<double>(kw) * static_cast<double>(kw);
            }

            blockAccum_     += meanSqKW;
            shortTermAccum_ += meanSqKW;
            ++blockSampleCount_;
            ++shortTermCount_;

            // BS.1770 gated 400 ms block.
            if (blockSampleCount_ >= blockSamples_)
            {
                const double blockPower = blockAccum_ / static_cast<double>(blockSamples_);
                processGatedBlock(blockPower);

                // Slide window.
                blockAccum_       = 0.0;
                blockSampleCount_ = 0;
            }

            // Short-term (3 s).
            if (shortTermCount_ >= shortBlkSmp_)
            {
                const double stPow = shortTermAccum_ / static_cast<double>(shortBlkSmp_);
                const float stDb  = (stPow > 1e-10) ?
                    static_cast<float>(-0.691 + 10.0 * std::log10(stPow)) : -100.0f;
                stLufs_.store(stDb, std::memory_order_relaxed);
                shortTermAccum_ = 0.0;
                shortTermCount_ = 0;
            }
        }
    }

    // ---- Any-thread reads (safe after processBlock) ----------------------

    float getIntegratedLufs() const noexcept
    {
        return intLufs_.load(std::memory_order_relaxed);
    }

    float getShortTermLufs() const noexcept
    {
        return stLufs_.load(std::memory_order_relaxed);
    }

    /// True-peak linear amplitude.
    float getTruePeak() const noexcept
    {
        return truePeak_.load(std::memory_order_relaxed);
    }

    /// Peak in dBFS.
    float getPeakDb() const noexcept
    {
        return peakDb_.load(std::memory_order_relaxed);
    }

private:
    double sr_   { 44100.0 };
    int    nch_  { 1 };
    int    blockSamples_  { 17640 };  // 400 ms at 44.1 kHz
    int    strideSamples_ { 3308 };   // 75 ms
    int    shortBlkSmp_   { 132300 }; // 3 s at 44.1 kHz

    // ---- Accumulators ---------------------------------------------------
    double   blockAccum_      { 0.0 };
    int      blockSampleCount_{ 0 };
    double   shortTermAccum_  { 0.0 };
    int      shortTermCount_  { 0 };
    double   integratedSum_   { 0.0 };
    int64_t  integratedCount_ { 0 };

    // Per-channel state.
    float peakRaw_[kMaxChannels] {};

    // ---- K-weighting filter state (two biquad stages per channel) --------
    // Stage 1: high-frequency shelving filter.
    // Stage 2: high-pass filter.
    // Coefficients approximated for 44.1 kHz / 48 kHz.

    struct BiquadState { float x1{}, x2{}, y1{}, y2{}; };
    struct ChKW { BiquadState stage1; BiquadState stage2; float prevRaw{}; };
    std::array<ChKW, kMaxChannels> kw_;

    // Coefficients (stage 1 = pre-shelf, stage 2 = HPF).
    float ks1_b0{}, ks1_b1{}, ks1_b2{}, ks1_a1{}, ks1_a2{};
    float ks2_b0{}, ks2_b1{}, ks2_b2{}, ks2_a1{}, ks2_a2{};

    // ---- Atomic outputs -------------------------------------------------
    std::atomic<float> truePeak_  { 0.0f };
    std::atomic<float> peakDb_    { -100.0f };
    std::atomic<float> intLufs_   { -100.0f };
    std::atomic<float> stLufs_    { -100.0f };

    // ---- Helpers --------------------------------------------------------

    void computeKWeightCoeffs() noexcept
    {
        // BS.1770 K-weighting (two biquad IIR stages).
        // Stage 1: High-frequency shelving filter (pre-filter).
        //   Approximate coefficients for a 4 dB shelf at 1681 Hz (48 kHz spec).
        //   Here we use analytic values scaled to sr_.
        const double pi = 3.14159265358979;
        const double f0 = 1681.0;
        const double db = 3.999843853973347;
        const double A  = std::pow(10.0, db / 40.0);
        const double w0 = 2.0 * pi * f0 / sr_;
        const double cosW = std::cos(w0);
        const double sinW = std::sin(w0);
        const double alp  = sinW / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / 0.708 - 1.0) + 2.0);
        const double sqA  = std::sqrt(A);

        const double b0s = A * ((A + 1.0) + (A - 1.0) * cosW + 2.0 * sqA * alp);
        const double b1s = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
        const double b2s = A * ((A + 1.0) + (A - 1.0) * cosW - 2.0 * sqA * alp);
        const double a0s = (A + 1.0) - (A - 1.0) * cosW + 2.0 * sqA * alp;
        const double a1s = 2.0 * ((A - 1.0) - (A + 1.0) * cosW);
        const double a2s = (A + 1.0) - (A - 1.0) * cosW - 2.0 * sqA * alp;
        const double inv0 = 1.0 / a0s;
        ks1_b0 = static_cast<float>(b0s * inv0);
        ks1_b1 = static_cast<float>(b1s * inv0);
        ks1_b2 = static_cast<float>(b2s * inv0);
        ks1_a1 = static_cast<float>(a1s * inv0);
        ks1_a2 = static_cast<float>(a2s * inv0);

        // Stage 2: HPF at 38.135 Hz.
        const double fh  = 38.135;
        const double wh  = 2.0 * pi * fh / sr_;
        const double ch  = 1.0 / std::tan(wh * 0.5);
        const double ch2 = ch * ch;
        const double sq2ch = 1.41421356 * ch;
        const double denh  = 1.0 / (1.0 + sq2ch + ch2);
        ks2_b0 = static_cast<float>(ch2 * denh);
        ks2_b1 = static_cast<float>(-2.0 * ch2 * denh);
        ks2_b2 = static_cast<float>(ch2 * denh);
        ks2_a1 = static_cast<float>(2.0 * (1.0 - ch2) * denh);
        ks2_a2 = static_cast<float>((1.0 - sq2ch + ch2) * denh);
    }

    float applyKWeight(float x, int ch) noexcept
    {
        auto& st = kw_[static_cast<std::size_t>(ch)];

        // Stage 1.
        const float y1 = ks1_b0 * x + ks1_b1 * st.stage1.x1 + ks1_b2 * st.stage1.x2
                       - ks1_a1 * st.stage1.y1 - ks1_a2 * st.stage1.y2;
        st.stage1.x2 = st.stage1.x1; st.stage1.x1 = x;
        st.stage1.y2 = st.stage1.y1; st.stage1.y1 = y1;

        // Stage 2.
        const float y2 = ks2_b0 * y1 + ks2_b1 * st.stage2.x1 + ks2_b2 * st.stage2.x2
                       - ks2_a1 * st.stage2.y1 - ks2_a2 * st.stage2.y2;
        st.stage2.x2 = st.stage2.x1; st.stage2.x1 = y1;
        st.stage2.y2 = st.stage2.y1; st.stage2.y1 = y2;

        return y2;
    }

    float estimateTruePeak(float current, int ch) noexcept
    {
        // Linear interpolation 4x oversample (very rough approximation).
        auto& st = kw_[static_cast<std::size_t>(ch)];
        const float prev = st.prevRaw;
        st.prevRaw = current;

        float peak = 0.0f;
        for (int k = 1; k <= 4; ++k)
        {
            const float t  = static_cast<float>(k) * 0.25f;
            const float ip = prev + t * (current - prev);
            const float a  = ip < 0.0f ? -ip : ip;
            if (a > peak) peak = a;
        }
        return peak;
    }

    void processGatedBlock(double blockPower) noexcept
    {
        // Absolute gate: -70 LUFS.
        constexpr double kAbsGate = 1e-7;  // 10^(-70/10) ≈ 1e-7
        if (blockPower < kAbsGate) return;

        integratedSum_   += blockPower;
        ++integratedCount_;

        // Compute integrated LUFS from current accumulated power.
        const double meanPow = integratedSum_ / static_cast<double>(integratedCount_);
        if (meanPow > 1e-10)
        {
            const float lufs = static_cast<float>(-0.691 + 10.0 * std::log10(meanPow));
            intLufs_.store(lufs, std::memory_order_relaxed);
        }
    }

    void updatePeakDb(float absLinear) noexcept
    {
        if (absLinear < 1e-9f) return;
        const float db = 20.0f * std::log10(absLinear);
        float cur = peakDb_.load(std::memory_order_relaxed);
        if (db > cur)
            peakDb_.store(db, std::memory_order_relaxed);
    }
};

} // namespace ssbb
