# Sing Song Bing Bong

A voice-first desktop music-production project for recording, beat creation, synthesis, arrangement, mixing, and mastering assistance.

## Product direction

The goal is an original production environment that combines:

- multitrack vocal and instrument recording
- MIDI sequencing and a 16-pad drum machine
- modular synth and sampler building blocks
- an original vocal chain for clear, confident, upfront rap delivery
- professional routing, metering, mixing, and WAV export
- an AI assistant that analyzes audio offline and proposes explainable, undoable changes

The project takes inspiration from broad West Coast production qualities—confident cadence, punchy drums, warm bass, clarity, space, and storytelling—without cloning or impersonating any artist's voice.

## Current status

The repository now includes the original safe-recording foundation plus a PRO song-sketching workflow:

- [Architecture](ARCHITECTURE.md)
- [Roadmap](ROADMAP.md)
- [Agent rules](AGENTS.md)
- a repository-level Studio Architect agent under `.github/agents/`
- GitHub Copilot repository instructions under `.github/copilot-instructions.md`
- a pinned JUCE/CMake Windows app target and CI workflow
- guided intro/verse/hook/outro rap structures
- a tempo-locked 16-step kick/snare/hat sequencer with two original presets
- an original vocal chain and mastering chain built with `juce::dsp`
- live vocal recording through the existing timestamped-take subsystem
- text-to-vocal preview generation on a worker thread, with an optional ONNX Runtime backend
- offline WAV rendering of the full mix, vocal stem, and instrument stem

The app preserves the safe C++20/JUCE recording architecture and keeps DSP deterministic and AI/file work off the real-time callback.

## Build bootstrap

### JUCE pin and licence path

- JUCE version: `8.0.14`
- Pinned JUCE commit: `2cdfca8feb300fb424002ba2c2751569e5bacb64`
- Pinned JUCE archive SHA-256: `ceb18e4ac9ab5ea71f3f20240d5852707767a1789ab43d06656a296da9e62f3e`
- Repository licence path for this public bootstrap: JUCE's AGPLv3 option
- Closed-source distribution requirement: obtain and document a commercial JUCE 8 licence before shipping non-AGPL binaries

### Windows configure, build, and test

Run these commands from a Windows developer shell with CMake 3.31+ and MSVC available:

```bash
cmake --preset windows-msvc
cmake --build --preset windows-debug --parallel
ctest --preset windows-test-debug --output-on-failure
```

The standalone target opens the full studio UI and exposes JUCE's audio-device selector below the production panels.

### Optional ONNX Runtime backend

The default build is usable without model files and produces a clearly synthetic preview voice. To compile the model bridge, provide a licensed ONNX Runtime C/C++ SDK and enable the option:

```bash
cmake --preset windows-msvc -DSSBB_ENABLE_ONNX=ON -DONNXRUNTIME_ROOT=C:/sdk/onnxruntime
```

Place licensed, consent-safe `acoustic.onnx` and `vocoder.onnx` files in a `models` directory beside the executable. Model weights are intentionally ignored by Git. The bridge documents its expected tensor names so it can be adapted to the chosen model contract. This project does not provide voice cloning, identity imitation, or artist impersonation.

## Working with the agent

Use the **Studio Architect** custom agent in GitHub Copilot for planning and implementing roadmap issues. Give it one bounded issue at a time and review every pull request before merging. The agent is instructed to protect recordings, keep AI work off the real-time audio thread, test DSP changes, respect licensing, and avoid voice impersonation.

## Principles

- Your recordings stay yours.
- AI suggestions require approval.
- Audio edits are non-destructive.
- Real-time stability comes before feature count.
- Only owned, licensed, or permissively licensed assets belong in the project.
- No claim is made that software replaces accredited engineers, teachers, or schools.
