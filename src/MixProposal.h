#pragma once
// MixProposal.h — explainable, undoable mix-assistant proposal.
//
// A MixProposal carries:
//   - A list of ParameterChange items (target ID + value + rationale).
//   - A human-readable explanation of why each change is suggested.
//   - An undo token that the CommandHistory can use to reverse the changes.
//   - A confidence score (0.0–1.0).
//
// Proposals are created by MixAssistant on the worker thread, then handed to
// the message thread for user approval.  The proposal is applied only after
// the user explicitly approves it.  All changes are wrapped in a CommandHistory
// ICommand so undo/redo works normally.
//
// Privacy:
//   PrivacyMode::LocalOnly  — no data leaves the machine.
//   PrivacyMode::CloudAllowed — the assistant may upload feature summaries
//                              (never raw audio) to an opt-in cloud provider.
//
// MESSAGE THREAD ONLY for apply() / undo().
// WORKER THREAD for MixAssistant::analyse().

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ssbb {

// ---- Privacy controls ---------------------------------------------------

enum class PrivacyMode : int
{
    LocalOnly    = 0,   ///< Analysis runs locally; nothing is uploaded.
    CloudAllowed = 1,   ///< Feature summaries (not raw audio) may be uploaded.
};

// ---- Individual parameter change ----------------------------------------

struct ParameterChange
{
    std::string parameterId;   ///< Stable ID matching the automation lane name.
    float       currentValue;  ///< Value before the change (for undo).
    float       proposedValue; ///< Value the assistant recommends.
    std::string rationale;     ///< One-sentence human-readable explanation.
    float       confidence;    ///< 0.0–1.0 confidence in this specific change.
};

// ---- Mastering target ----------------------------------------------------

enum class DeliveryPlatform : int
{
    Streaming   = 0,   ///< Spotify/Apple Music: -14 LUFS, -1.0 dBTP.
    CD          = 1,   ///< CD: -9 LUFS, -0.3 dBTP.
    YouTube     = 2,   ///< YouTube: -14 LUFS, -1.0 dBTP.
    Club        = 3,   ///< Club/DJ: -8 LUFS, -0.3 dBTP.
    Podcast     = 4,   ///< Podcast: -16 LUFS, -1.0 dBTP.
    Custom      = 5,   ///< User-specified.
};

struct MasteringTarget
{
    DeliveryPlatform platform      { DeliveryPlatform::Streaming };
    float            targetLufs   { -14.0f };
    float            targetTpDb   { -1.0f };
    std::string      platformName { "Streaming" };
};

inline MasteringTarget masteringTargetFor(DeliveryPlatform p)
{
    switch (p)
    {
        case DeliveryPlatform::CD:
            return { p, -9.0f,  -0.3f, "CD" };
        case DeliveryPlatform::YouTube:
            return { p, -14.0f, -1.0f, "YouTube" };
        case DeliveryPlatform::Club:
            return { p, -8.0f,  -0.3f, "Club" };
        case DeliveryPlatform::Podcast:
            return { p, -16.0f, -1.0f, "Podcast" };
        case DeliveryPlatform::Streaming:
        default:
            return { p, -14.0f, -1.0f, "Streaming" };
    }
}

// ---- MixProposal ---------------------------------------------------------

struct MixProposal
{
    /// Unique ID for tracking / logging.  Never contains user audio.
    uint64_t id { 0 };

    /// Human-readable summary of the proposal.
    std::string summary;

    /// Individual parameter changes.
    std::vector<ParameterChange> changes;

    /// Mastering target the proposal is aimed at.
    MasteringTarget target;

    /// Overall confidence of the proposal (0.0–1.0).
    float confidence { 0.0f };

    /// Set to true when the user approves.  False = pending / rejected.
    bool approved { false };

    /// Whether any data was sent off-device to produce this proposal.
    PrivacyMode privacyMode { PrivacyMode::LocalOnly };

    // ---- Undo support ---------------------------------------------------
    //
    // apply() calls applyFn (sets parameters), stores undoFn for later.
    // undo() calls undoFn to restore the previous values.

    /// Apply the proposed changes.  Must be called from the message thread.
    /// Returns false if the proposal has already been applied or is not approved.
    bool apply()
    {
        if (!approved || applied_) return false;
        if (applyFn) applyFn();
        applied_ = true;
        return true;
    }

    /// Undo the applied changes.  Must be called from the message thread.
    bool undoApply()
    {
        if (!applied_) return false;
        if (undoFn) undoFn();
        applied_ = false;
        return true;
    }

    bool isApplied() const noexcept { return applied_; }

    /// Callback that applies parameter changes (injected by MixAssistant).
    std::function<void()> applyFn;

    /// Callback that restores parameters to their pre-proposal values.
    std::function<void()> undoFn;

private:
    bool applied_ { false };
};

} // namespace ssbb
