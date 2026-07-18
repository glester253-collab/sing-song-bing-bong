#pragma once
// DrumPad.h — data model for a single velocity-sensitive drum pad.
//
// Each pad maps a note number and optional sample path to a trigger slot.
// Pad hits are timestamped in timeline samples for sequencer alignment.
//
// MESSAGE THREAD: configuration (name, notePitch, samplePath).
// AUDIO THREAD:   pad trigger events are delivered via PadEngine.
//                 DrumPad itself carries no audio-thread state.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ssbb {

/// A single drum pad definition.  16 of these make up a standard pad bank.
struct DrumPad
{
    static constexpr int kNumPads = 16;

    /// MIDI note number for this pad (0–127).
    int notePitch { 60 };

    /// Human-readable name (e.g. "Kick", "Snare", "HH Open").
    std::string name;

    /// Optional path to a WAV sample file.  Empty = MIDI-only (no preview audio).
    std::filesystem::path samplePath;

    /// Default velocity used by the step sequencer when the step is active
    /// but no per-step velocity override is set (0–127).
    int defaultVelocity { 100 };
};

/// A timestamped pad-hit event placed in the SPSC queue between the audio thread
/// (producer) and the MIDI / sequencer layer (consumer).
struct PadHitEvent
{
    int     padIndex    { 0 };   ///< Which pad was hit (0–15).
    int     velocity    { 0 };   ///< Velocity 0–127.
    int64_t sampleTime  { 0 };   ///< Transport position in samples at the hit.
};

} // namespace ssbb
