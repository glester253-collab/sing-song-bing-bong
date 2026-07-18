# Roadmap

## Milestone 0 — Foundation

- [x] Pin JUCE after license review and create the CMake project
- [x] Add Windows CI configure/build/test
- [x] Add coding, formatting, sanitizer, and release conventions
- [x] Create the session schema and migration strategy
- [ ] Create audio-device diagnostics and a latency test

## Milestone 1 — Record a vocal safely

- [x] Audio/MIDI device selection (persistence across restarts)
- [x] Transport, tempo, time signature, metronome, loop
- [ ] Arm, monitor, record, stop, and play one mono track
- [x] Timestamped takes, autosave, and crash recovery
- [ ] Waveform cache and non-destructive trim/move
- [ ] WAV import and export

## Milestone 2 — Produce a beat

- [ ] 16 velocity-sensitive pads
- [ ] Sample browser with owned/licensed asset checks
- [ ] 16/32/64-step sequencer, swing, probability, and per-step velocity
- [ ] MIDI recording and quantization
- [ ] Subtractive synth with modulation matrix
- [ ] Track freeze/bounce

## Milestone 3 — Mix vocals and music

- [ ] Gain, pan, mute, solo, buses, pre/post sends, and meters
- [ ] Original vocal chain: HPF, EQ, de-esser, compression, saturation, delay, limiter
- [ ] Automation lanes and parameter smoothing
- [ ] Loudness, true-peak, phase, and spectrum metering
- [ ] Reference-track import without unauthorized copying
- [ ] Mix snapshots and A/B comparison

## Milestone 4 — Assisted mix and master

- [ ] Offline feature extraction
- [ ] Explainable gain-staging suggestions
- [ ] Explainable EQ/dynamics suggestions
- [ ] Mastering targets by delivery platform
- [ ] Undoable proposal/approval workflow
- [ ] Privacy controls for local versus cloud analysis

## Milestone 5 — Extensibility and release

- [ ] Evaluate VST3/CLAP/AU hosting and licensing
- [ ] Controller mapping and MIDI learn
- [ ] Plugin/module crash isolation
- [ ] Project templates and preset browser
- [ ] Installer, signed builds, diagnostics bundle, and recovery documentation
- [ ] Performance, accessibility, security, and license audit

## Not in the first release

- voice cloning or impersonation
- unapproved autonomous changes to a mix
- copyrighted curriculum ingestion
- bundled commercial samples or proprietary plugin SDKs
- claims of replacing trained engineers, teachers, or accredited schools