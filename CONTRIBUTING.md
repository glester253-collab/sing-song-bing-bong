# Contributing to Sing Song Bing Bong

Thank you for your interest in contributing! This document covers the conventions, workflow, and audio-thread rules you must follow. Read it before opening a pull request.

---

## Before you start

1. Read [`ARCHITECTURE.md`](ARCHITECTURE.md) — especially the thread model and audio-safety rules.
2. Read [`ROADMAP.md`](ROADMAP.md) — target one roadmap item per pull request when practical.
3. Read [`AGENTS.md`](AGENTS.md) — required behaviour for AI agents working in this repo.

---

## Non-negotiable audio rules

The real-time audio thread is the highest-priority safety boundary in this codebase. **Any violation is a blocking review defect.**

| Rule | Why it matters |
|------|---------------|
| No heap allocation | `malloc`/`new`/`std::vector::push_back` can block on the OS allocator |
| No mutexes or locks | Can block for an unbounded duration |
| No file or network I/O | Can block for an unbounded duration |
| No JUCE `String` construction | Allocates on the heap |
| No logging | Typically allocates |
| No exceptions | Thrown exceptions can allocate and are non-deterministic |
| No AI inference | Latency is unbounded and model memory is large |
| No virtual dispatch to unknown code | Cannot statically verify the above |

Audio-thread-only paths are `AudioEngine::audioDeviceIOCallbackWithContext`, `Transport::process`, `Metronome::processBlock`, and `VocalTrack::processBlock`. Keep them allocation-free, lock-free, and I/O-free.

---

## Code style

This project targets **C++20** with JUCE 8. Style is enforced by `.clang-format` and checked by `.clang-tidy`.

### Formatting

```bash
# Format a single file
clang-format -i src/MyFile.cpp

# Check formatting without modifying
clang-format --dry-run --Werror src/MyFile.cpp
```

### Key conventions

- **Types** — `PascalCase` (classes, structs, enums)
- **Functions/methods** — `camelCase`
- **Private/protected members** — `camelCase_` (trailing underscore)
- **Local variables and parameters** — `camelCase`
- **Constants** — `kCamelCase`
- **Namespaces** — `snake_case`; all project code lives in `namespace ssbb`
- **Includes** — grouped: JUCE headers, then standard library, then local project headers (clang-format enforces order)
- **Comments** — match the existing style: full sentences for doc-comments above declarations, inline `//` for implementation notes

---

## Building

### Linux (CI / no GUI)

```bash
cmake -B build/linux-test \
      -DBUILD_TESTING=ON \
      -DSSBB_BUILD_STANDALONE_APP=OFF \
      -DCMAKE_CXX_STANDARD=20
cmake --build build/linux-test
ctest --test-dir build/linux-test -V
```

### Linux with sanitizers

```bash
cmake --preset linux-sanitize
cmake --build build/linux-sanitize
ctest --test-dir build/linux-sanitize -V
```

### Windows

```bash
cmake --preset windows-msvc
cmake --build --preset windows-debug
ctest --preset windows-test-debug
```

---

## Testing requirements

- Every audio DSP change requires deterministic tests (no JUCE needed for pure-C++20 modules).
- Tests live in `tests/` and follow the `expect(condition, message, success)` pattern from `transport_tests.cpp`.
- Check `silence`, `impulse`, `NaN/Inf`, and `denormal` cases for any DSP path.
- Check sample rates `44100`, `48000`, and `96000 Hz` for timing-dependent code.
- Check block sizes `64`, `128`, `256`, `512`, and `1024` samples where relevant.
- Session round-trip (save → load → compare) is required for any session-model change.

---

## Pull request checklist

Before marking a PR ready for review:

- [ ] `cmake --build` succeeds without warnings on the CI preset
- [ ] All existing tests pass (`ctest -V`)
- [ ] New tests added for any new DSP, timing, or serialisation logic
- [ ] No audio-thread allocation, locking, I/O, or logging introduced
- [ ] Every destructive user action is undoable or requires explicit confirmation
- [ ] `clang-format` applied to all changed `.cpp`/`.h` files
- [ ] PR description states: acceptance criteria, audio-thread risk assessment, test commands, and (for UI changes) a screenshot or description of manual listening checks
- [ ] No secrets, proprietary SDKs, commercial samples, or unlicensed model weights committed
- [ ] ROADMAP.md item checked off if a roadmap issue is fully addressed

---

## Commit messages

Use the [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) format:

```
<type>(<scope>): <short summary>

[optional body]
[optional BREAKING CHANGE footer]
```

Types: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `perf`, `ci`.

Examples:
```
feat(vocal-track): add non-destructive trim/move for clips
fix(metronome): correct click placement across loop boundary
test(transport): add denormal-flush and NaN rejection cases
```

---

## What we do not accept

- Voice cloning or artist impersonation features
- Copyrighted curriculum, commercial samples, or proprietary SDK binaries
- Autonomous mix changes without explicit user approval
- Claims of replacing trained engineers, teachers, or accredited schools
- Any change that violates the audio-thread safety rules above

If you are unsure whether a feature idea is in scope, open a discussion issue before writing code.
