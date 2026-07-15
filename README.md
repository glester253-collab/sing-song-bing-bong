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

Foundation and architecture stage. The repository now includes:

- [Architecture](ARCHITECTURE.md)
- [Roadmap](ROADMAP.md)
- [Agent rules](AGENTS.md)
- a repository-level Studio Architect agent under `.github/agents/`
- GitHub Copilot repository instructions under `.github/copilot-instructions.md`

The first engineering milestone is a safe Windows desktop prototype that records and plays back one vocal track using C++20, JUCE, and CMake.

## Working with the agent

Use the **Studio Architect** custom agent in GitHub Copilot for planning and implementing roadmap issues. Give it one bounded issue at a time and review every pull request before merging. The agent is instructed to protect recordings, keep AI work off the real-time audio thread, test DSP changes, respect licensing, and avoid voice impersonation.

## Principles

- Your recordings stay yours.
- AI suggestions require approval.
- Audio edits are non-destructive.
- Real-time stability comes before feature count.
- Only owned, licensed, or permissively licensed assets belong in the project.
- No claim is made that software replaces accredited engineers, teachers, or schools.