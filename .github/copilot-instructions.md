# Repository instructions

Sing Song Bing Bong is a voice-first desktop music-production application in its foundation stage. The target stack is C++20, JUCE, and CMake. The first supported development platform is Windows; keep platform boundaries explicit so macOS support can follow.

Before changing code, read `ARCHITECTURE.md`, `ROADMAP.md`, and the root `AGENTS.md`.

## Non-negotiable audio rules

- The real-time audio thread must not allocate memory, acquire locks, access files or networks, log, throw exceptions, or run AI inference.
- Prepare DSP resources before playback and safely handle sample-rate, block-size, and channel-layout changes.
- Use stable parameter IDs, smooth audible parameter changes, flush denormals, and reject NaN/Inf.
- Preserve recordings with non-destructive edits, timestamped takes, autosave, and crash recovery.
- Add deterministic tests for DSP, timing, serialization, and session migration.
- Keep AI features offline from the audio callback. AI may analyze rendered or copied buffers and must return an explainable, undoable proposal.
- Never commit secrets, proprietary SDKs, copyrighted course workbooks, commercial samples, or unlicensed model weights.
- Do not implement voice cloning or artist impersonation. Use original processing and broad production attributes.

## Pull request expectations

Keep changes small and coherent. State acceptance criteria, audio-thread risks, test commands, manual listening checks, screenshots for UI work, and known limitations. Do not merge or publish releases without owner approval.