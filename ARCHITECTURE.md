# Architecture

## Product shape

A native, voice-first desktop music-production application. The first milestone targets Windows and establishes portable C++ boundaries for later macOS support.

## Technology baseline

- C++20
- JUCE with CMake
- Catch2 or JUCE UnitTest for deterministic tests
- GitHub Actions for configure, build, and test
- Optional cloud or local AI provider behind an interface; no provider is required for core recording

Exact JUCE and third-party versions must be pinned after license review. Proprietary plugin SDKs, commercial samples, and model weights are not stored in this repository.

## Layers

```
App/UI
  Timeline | Mixer | Pads | Synth | Vocal Chain | Assistant
                         |
Command + Undo + Session Model
                         |
Audio Engine
  Transport | Graph | Tracks | Buses | Automation | Render
                         |
DSP Modules
  EQ | Dynamics | Delay | Saturation | Synth | Sampler | Metering
                         |
Device + File Adapters
  Audio/MIDI devices | WAV sessions/takes | Crash recovery
```

## Thread model

### Audio thread

Processes preallocated buffers and immutable/prepared state only. No locks, allocation, file access, networking, logging, exceptions, or AI inference.

### Message thread

Owns UI state, commands, undo history, and user interaction.

### Worker pool

Handles waveform generation, file import/export, offline analysis, autosave preparation, and AI requests. Results cross into the engine through bounded queues and prepared state swaps.

## Session model

A versioned session contains tracks, clips, source-file references, transport settings, routing, automation, plugin/module state, and undo metadata. Audio edits are non-destructive. Recordings are immutable timestamped takes stored outside autosave metadata.

## First vertical slices

1. Device selection, transport, metronome, and safe audio shutdown.
2. Record one mono vocal track and play it back.
3. Multitrack timeline with non-destructive trim/move.
4. Mixer with gain, pan, mute, solo, meter, and master limiter.
5. Vocal chain with safe defaults and parameter smoothing.
6. Drum pads plus a tempo-locked 16-step sequencer.
7. Synth instrument and MIDI recording.
8. Offline mix assistant returning explainable, undoable proposals.
9. WAV render with peak and integrated-loudness report.

## Original vocal-chain target

Start with an original chain built for clear, confident rap vocals:

1. polarity and input trim
2. high-pass filter
3. subtractive/clarity EQ
4. de-esser
5. compressor
6. gentle saturation
7. presence/air EQ
8. short stereo delay send
9. peak limiter

Presets are starting points, not claims of reproducing any named artist, studio, school, console, or engineer.

## AI boundary

The assistant may inspect session metadata and offline feature summaries such as peak, RMS, loudness, spectral balance, crest factor, and dynamics. It returns a proposal containing rationale, confidence, parameter changes, and an undo token. The user approves changes before they affect a session. Raw vocals are not uploaded without explicit opt-in.

## Definition of done for audio features

- automated tests pass
- no audio-thread allocations or locks in the changed path
- sample rates 44.1/48/96 kHz checked where relevant
- block sizes 64/128/256/512/1024 checked where relevant
- silence, impulse, tone, denormal, NaN/Inf, and rapid-automation cases considered
- session round-trip verified
- manual listening check documented
- recording preservation confirmed