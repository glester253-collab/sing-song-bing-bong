#pragma once
// MixAssistant.h — offline mix/master analysis and proposal generator.
//
// WORKER THREAD: analyse() analyses a feature set and produces an explainable,
//                undoable MixProposal.
// MESSAGE THREAD: the returned proposal is presented to the user for approval.
//                 Approved proposals are applied via CommandHistory.
//
// Privacy rule:
//   - Only FeatureSet summaries (no raw audio) are used here.
//   - If privacy == LocalOnly, analysis runs entirely in-process.
//   - If privacy == CloudAllowed, a cloud provider interface may be called
//     (stub here — real implementation requires explicit opt-in and a provider).
//
// Gain-staging suggestions use the BS.1770 integrated loudness difference
// from the target LUFS.  EQ suggestions are based on spectral balance ratios
// and a simple genre-neutral reference curve.

#include "FeatureExtractor.h"
#include "MixProposal.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ssbb {

class MixAssistant
{
public:
    MixAssistant() = default;

    // ---- Worker-thread API ----------------------------------------------

    /// Analyse `features` against `target` and return an explainable proposal.
    /// `getParamFn(id)` returns the current parameter value for a given ID.
    /// `privacyMode` controls whether cloud analysis may be used.
    MixProposal analyse(const FeatureSet&                    features,
                        const MasteringTarget&               target,
                        std::function<float(const std::string&)> getParamFn,
                        PrivacyMode                          privacyMode = PrivacyMode::LocalOnly) const
    {
        if (!features.valid) return MixProposal{};

        static std::atomic<uint64_t> sNextId { 1 };
        MixProposal proposal;
        proposal.id          = sNextId.fetch_add(1, std::memory_order_relaxed);
        proposal.target      = target;
        proposal.privacyMode = privacyMode;
        proposal.confidence  = 0.0f;

        float totalConf = 0.0f;
        int   nChanges  = 0;

        // ---- 1. Gain-staging suggestion ---------------------------------
        // Recommend output-gain trim to reach target LUFS.
        if (features.integratedLufs > -90.0f)
        {
            const float delta = target.targetLufs - features.integratedLufs;
            if (std::abs(delta) > 0.5f)  // only suggest if > 0.5 LU off
            {
                const float currentGain = getParamFn ? getParamFn("output_gain_db") : 0.0f;
                const float proposedGain = currentGain + delta;

                ParameterChange pc;
                pc.parameterId   = "output_gain_db";
                pc.currentValue  = currentGain;
                pc.proposedValue = proposedGain;
                pc.confidence    = 0.85f;
                pc.rationale     = "Integrated loudness is "
                    + formatLU(features.integratedLufs)
                    + "; target for " + target.platformName + " is "
                    + formatLU(target.targetLufs) + ".  Adjust by "
                    + formatLU(delta) + ".";
                proposal.changes.push_back(std::move(pc));
                totalConf += 0.85f;
                ++nChanges;
            }
        }

        // ---- 2. True-peak limiting suggestion ---------------------------
        if (features.truePeakLinear > 0.0f)
        {
            const float tpDb = 20.0f * std::log10(features.truePeakLinear + 1e-9f);
            if (tpDb > target.targetTpDb)
            {
                const float currentLimThr = getParamFn ? getParamFn("limiter_thresh_db") : -1.0f;
                const float proposed = target.targetTpDb;

                ParameterChange pc;
                pc.parameterId   = "limiter_thresh_db";
                pc.currentValue  = currentLimThr;
                pc.proposedValue = proposed;
                pc.confidence    = 0.90f;
                pc.rationale     = "True peak is " + formatDb(tpDb)
                    + " dBTP, exceeding the " + target.platformName
                    + " ceiling of " + formatDb(target.targetTpDb) + " dBTP.";
                proposal.changes.push_back(std::move(pc));
                totalConf += 0.90f;
                ++nChanges;
            }
        }

        // ---- 3. EQ balance suggestion -----------------------------------
        // Reference curve (genre-neutral): low ≈ 30%, mid ≈ 55%, high ≈ 15%.
        constexpr float kRefLow  = 0.30f;
        constexpr float kRefHigh = 0.15f;

        if (features.lowEnergyRatio > kRefLow + 0.10f)
        {
            const float cur  = getParamFn ? getParamFn("low_shelf_db") : 0.0f;
            const float diff = -(features.lowEnergyRatio - kRefLow) * 10.0f;

            ParameterChange pc;
            pc.parameterId   = "low_shelf_db";
            pc.currentValue  = cur;
            pc.proposedValue = std::max(-12.0f, cur + diff);
            pc.confidence    = 0.65f;
            pc.rationale     = "Low-frequency energy (" +
                formatPct(features.lowEnergyRatio) +
                ") is above the reference (" + formatPct(kRefLow) +
                "). Consider a low-shelf cut to reduce muddiness.";
            proposal.changes.push_back(std::move(pc));
            totalConf += 0.65f;
            ++nChanges;
        }

        if (features.highEnergyRatio < kRefHigh - 0.05f)
        {
            const float cur  = getParamFn ? getParamFn("air_shelf_db") : 0.0f;
            const float diff = (kRefHigh - features.highEnergyRatio) * 15.0f;

            ParameterChange pc;
            pc.parameterId   = "air_shelf_db";
            pc.currentValue  = cur;
            pc.proposedValue = std::min(12.0f, cur + diff);
            pc.confidence    = 0.60f;
            pc.rationale     = "High-frequency energy (" +
                formatPct(features.highEnergyRatio) +
                ") is below the reference (" + formatPct(kRefHigh) +
                "). An air-shelf boost may add clarity and presence.";
            proposal.changes.push_back(std::move(pc));
            totalConf += 0.60f;
            ++nChanges;
        }

        // ---- 4. Dynamics suggestion -------------------------------------
        if (features.crestFactorDb < 6.0f)
        {
            ParameterChange pc;
            pc.parameterId   = "comp_ratio";
            pc.currentValue  = getParamFn ? getParamFn("comp_ratio") : 4.0f;
            pc.proposedValue = pc.currentValue * 0.8f;
            pc.confidence    = 0.55f;
            pc.rationale     = "Crest factor is " + formatDb(features.crestFactorDb)
                + " dB, suggesting significant limiting or heavy compression.  "
                "Reducing the compressor ratio may restore transient punch.";
            proposal.changes.push_back(std::move(pc));
            totalConf += 0.55f;
            ++nChanges;
        }

        // ---- Build summary and overall confidence -----------------------
        if (nChanges == 0)
        {
            proposal.summary    = "Mix levels look good for " + target.platformName
                                + ". No changes suggested.";
            proposal.confidence = 1.0f;
        }
        else
        {
            proposal.confidence = totalConf / static_cast<float>(nChanges);
            proposal.summary    = "Found " + std::to_string(nChanges)
                + " suggestion(s) to optimise for " + target.platformName
                + " (average confidence: "
                + formatPct(proposal.confidence) + ").";
        }

        return proposal;
    }

private:
    static std::string formatDb(float db)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(db));
        return std::string(buf);
    }

    static std::string formatLU(float lu)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f LUFS", static_cast<double>(lu));
        return std::string(buf);
    }

    static std::string formatPct(float v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(v * 100.0f));
        return std::string(buf);
    }
};

} // namespace ssbb
