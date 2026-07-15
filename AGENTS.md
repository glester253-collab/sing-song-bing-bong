# AGENTS.md

## Project

Sing Song Bing Bong is a voice-first music-production application. Build it as a professional, original production tool—not as a voice-cloning or artist-impersonation system.

## Before working

Read:

1. `ARCHITECTURE.md`
2. `ROADMAP.md`
3. `.github/copilot-instructions.md`

## Required behavior

- Protect user recordings and session files above all else.
- Keep real-time DSP deterministic, allocation-free, lock-free, and isolated from AI/network work.
- Make every destructive action undoable or require explicit confirmation.
- Use only owned, licensed, or permissively licensed audio, code, documentation, and model assets.
- Convert artist references into broad musical characteristics; never copy a real person's voice or falsely label output as that artist.
- Complete one roadmap issue per pull request when practical.
- Include builds/tests and manual listening checks in every pull request.
- Stop and ask when a license, user consent, or release decision is uncertain.