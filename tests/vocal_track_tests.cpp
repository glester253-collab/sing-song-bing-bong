// vocal_track_tests.cpp
// Plain C++20 — NO JUCE headers, NO third-party test frameworks.
// Follows the same expect() / main() pattern as transport_tests.cpp.
//
// Links: VocalTrack.cpp, TakeManager.cpp, WaveformCache.cpp, SessionDocument.cpp
// Header-only: RecordBuffer.h, Clip.h, WavWriter.h
//
// Tested areas:
//   1. RecordBuffer — write/read round-trip and overflow drop
//   2. Clip         — trimStart / trimEnd / moveTo / activeLengthSamples
//   3. TakeManager  — unique timestamped path generation
//   4. SessionDocument — save/load/saveRecovery/loadRecovery JSON round-trip
//   5. WaveformCache   — initial state (isReady == false, frames empty)

#include "RecordBuffer.h"
#include "Clip.h"
#include "TakeManager.h"
#include "SessionDocument.h"
#include "WaveformCache.h"
#include "PlaybackBuffer.h"
#include "WavWriter.h"
#include "VocalTrack.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message, bool& success)
{
    if (!condition)
    {
        std::cerr << "[FAIL] " << message << '\n';
        success = false;
    }
}

} // namespace

// ============================================================================
// Helper: create a minimal in-memory temp directory, remove on scope exit.
// ============================================================================
struct TempDir
{
    std::filesystem::path path;

    explicit TempDir(std::string_view name)
        : path(std::filesystem::temp_directory_path() / name)
    {
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        // Ignore error on cleanup — best-effort.
    }

    // Convenience: path to a file inside this directory.
    std::filesystem::path operator/(std::string_view name) const
    {
        return path / name;
    }
};

// ============================================================================
int main()
{
    bool success = true;

    // -------------------------------------------------------------------------
    // 1. RecordBuffer — write / read round-trip
    // -------------------------------------------------------------------------
    {
        // Use a small capacity for the test (power of 2, ≥ 2).
        ssbb::RecordBuffer<64> rb;

        // Buffer starts empty.
        expect(rb.availableRead() == 0,
               "RecordBuffer: initially empty (availableRead == 0)", success);
        expect(rb.availableWrite() == 64,
               "RecordBuffer: initially full write capacity (64)", success);

        // Write 32 samples with known values.
        float input[32];
        for (int i = 0; i < 32; ++i)
            input[i] = static_cast<float>(i) * 0.01f;

        const bool wrote = rb.write(input, 32);
        expect(wrote,
               "RecordBuffer: write 32 samples to capacity-64 → true", success);
        expect(rb.availableRead() == 32,
               "RecordBuffer: availableRead == 32 after write", success);

        // Read them back.
        float output[32] = {};
        const int nRead = rb.read(output, 32);
        expect(nRead == 32,
               "RecordBuffer: read returns 32", success);
        expect(rb.availableRead() == 0,
               "RecordBuffer: buffer empty after read", success);

        // Values must match exactly (float copy, no DSP).
        bool allMatch = true;
        for (int i = 0; i < 32; ++i)
            if (output[i] != input[i]) { allMatch = false; break; }
        expect(allMatch,
               "RecordBuffer: round-trip sample values match exactly", success);
    }

    // -------------------------------------------------------------------------
    // 1b. RecordBuffer — overflow / drop
    // -------------------------------------------------------------------------
    {
        ssbb::RecordBuffer<8> rb;
        float data[9] = {};

        // Fill the buffer completely.
        const bool ok1 = rb.write(data, 8);
        expect(ok1,
               "RecordBuffer: write 8 samples to capacity-8 → true", success);

        // A further write of even 1 sample must be refused (returns false).
        const bool ok2 = rb.write(data, 1);
        expect(!ok2,
               "RecordBuffer: write to full capacity-8 buffer → false (safe drop)", success);

        // The existing 8 samples must still be readable and correct (drop was
        // non-destructive).
        expect(rb.availableRead() == 8,
               "RecordBuffer: overflow drop does not corrupt existing data", success);
    }

    // -------------------------------------------------------------------------
    // 1c. RecordBuffer — zero-count edge cases
    // -------------------------------------------------------------------------
    {
        ssbb::RecordBuffer<16> rb;
        float dummy[1] = {};

        expect(rb.write(dummy, 0),
               "RecordBuffer: write(0) returns true (no-op)", success);
        expect(rb.read(dummy, 0) == 0,
               "RecordBuffer: read(0) returns 0 (no-op)", success);
        expect(rb.availableRead() == 0,
               "RecordBuffer: buffer still empty after no-op write/read", success);
    }

    // -------------------------------------------------------------------------
    // 2. Clip — non-destructive edits
    // -------------------------------------------------------------------------
    {
        ssbb::Clip c;
        c.sourceLengthSamples = 1000;

        // Initial state
        expect(c.trimStartSamples == 0,  "Clip: initial trimStart == 0", success);
        expect(c.trimEndSamples   == 0,  "Clip: initial trimEnd == 0",   success);
        expect(c.offsetSamples    == 0,  "Clip: initial offset == 0",    success);
        expect(c.activeLengthSamples() == 1000,
               "Clip: active length == source length when untrimmed", success);

        // trimStart
        c.trimStart(100);
        expect(c.trimStartSamples == 100,
               "Clip: trimStart(100) sets trimStartSamples", success);
        expect(c.activeLengthSamples() == 900,
               "Clip: activeLengthSamples == 1000-100 after trimStart", success);

        // trimEnd
        c.trimEnd(200);
        expect(c.trimEndSamples == 200,
               "Clip: trimEnd(200) sets trimEndSamples", success);
        expect(c.activeLengthSamples() == 700,
               "Clip: activeLengthSamples == 1000-100-200 == 700", success);

        // moveTo
        c.moveTo(48000);
        expect(c.offsetSamples == 48000,
               "Clip: moveTo(48000) sets offsetSamples", success);
        // Active length unchanged by move.
        expect(c.activeLengthSamples() == 700,
               "Clip: activeLengthSamples unchanged after moveTo", success);

        // sourceStartSample / sourceEndSample
        expect(c.sourceStartSample() == 100,
               "Clip: sourceStartSample == trimStartSamples", success);
        expect(c.sourceEndSample() == 800,
               "Clip: sourceEndSample == sourceLengthSamples - trimEndSamples", success);

        // Clamping: over-trim results in zero, not negative.
        ssbb::Clip c2;
        c2.sourceLengthSamples = 10;
        c2.trimStart(6);
        c2.trimEnd(6);
        expect(c2.activeLengthSamples() == 0,
               "Clip: activeLengthSamples clamps to 0 when over-trimmed", success);
    }

    // -------------------------------------------------------------------------
    // 3. TakeManager — unique timestamped paths
    // -------------------------------------------------------------------------
    {
        TempDir tmp("ssbb_test_take_manager");

        ssbb::TakeManager tm;

        const auto p1 = tm.createTakePath(tmp.path, 44100.0, 1);
        const auto p2 = tm.createTakePath(tmp.path, 44100.0, 1);

        // Different paths (counter suffix must differ even in the same second).
        expect(p1 != p2,
               "TakeManager: two rapid calls produce different paths", success);

        // Both paths are inside the requested directory.
        expect(p1.parent_path() == tmp.path,
               "TakeManager: path 1 is inside takeDir", success);
        expect(p2.parent_path() == tmp.path,
               "TakeManager: path 2 is inside takeDir", success);

        // Filename format: "take_YYYYMMDDTHHmmss_NNN.wav"
        const std::string fn1 = p1.filename().string();
        expect(fn1.substr(0, 5) == "take_",
               "TakeManager: filename starts with 'take_'", success);
        expect(fn1.size() >= 4 &&
               fn1.substr(fn1.size() - 4) == ".wav",
               "TakeManager: filename ends with '.wav'", success);
        // Basic length sanity: "take_" + 15 chars timestamp + "_" + 3 digits + ".wav" = 28.
        expect(fn1.size() >= 20,
               "TakeManager: filename has plausible length", success);

        // Metadata stored
        expect(tm.takes().size() == 2,
               "TakeManager: takes() has 2 entries after 2 calls", success);

        // Third call: even with a file created on disk for p1, must not collide.
        {
            // Touch p1 on disk (simulate a previous recording).
            std::ofstream f(p1, std::ios::trunc);
            f << "placeholder";
        }
        const auto p3 = tm.createTakePath(tmp.path, 44100.0, 1);
        expect(p3 != p1 && p3 != p2,
               "TakeManager: third call avoids both in-memory and on-disk collision",
               success);
    }

    // -------------------------------------------------------------------------
    // 4. SessionDocument — JSON save / load round-trip
    // -------------------------------------------------------------------------
    {
        TempDir tmp("ssbb_test_session_doc");
        const auto sessionPath = tmp / "test_session.json";

        // Build a session with one take and one clip.
        ssbb::SessionData orig;
        orig.version = ssbb::SessionDocument::kSchemaVersion;
        orig.dirty   = false;

        ssbb::TakeEntry te;
        te.path         = (tmp.path / "take_20260716T055324_001.wav").string();
        te.sampleRate   = 44100.0;
        te.numChannels  = 1;
        te.isoTimestamp = "20260716T055324";
        orig.takes.push_back(te);

        ssbb::ClipEntry ce;
        ce.takePath            = te.path;
        ce.offsetSamples       = 12345678LL;
        ce.trimStartSamples    = 100LL;
        ce.trimEndSamples      = 200LL;
        ce.sourceLengthSamples = 999999LL;
        orig.clips.push_back(ce);

        // Save
        const bool saved = ssbb::SessionDocument::save(sessionPath, orig);
        expect(saved,
               "SessionDocument: save() returns true", success);
        expect(std::filesystem::exists(sessionPath),
               "SessionDocument: file exists after save()", success);

        // Load and compare
        ssbb::SessionData loaded;
        const bool ok = ssbb::SessionDocument::load(sessionPath, loaded);
        expect(ok,
               "SessionDocument: load() returns true", success);

        expect(loaded.version == orig.version,
               "SessionDocument: version round-trips exactly", success);

        expect(loaded.takes.size() == 1,
               "SessionDocument: one take loaded", success);
        expect(loaded.clips.size() == 1,
               "SessionDocument: one clip loaded", success);

        if (!loaded.takes.empty())
        {
            const auto& lt = loaded.takes[0];
            expect(lt.path == te.path,
                   "SessionDocument: take path round-trips exactly", success);
            expect(lt.sampleRate == 44100.0,
                   "SessionDocument: sampleRate (44100.0) round-trips exactly", success);
            expect(lt.numChannels == 1,
                   "SessionDocument: numChannels round-trips exactly", success);
            expect(lt.isoTimestamp == te.isoTimestamp,
                   "SessionDocument: isoTimestamp round-trips exactly", success);
        }

        if (!loaded.clips.empty())
        {
            const auto& lc = loaded.clips[0];
            expect(lc.takePath == ce.takePath,
                   "SessionDocument: clip takePath round-trips exactly", success);
            expect(lc.offsetSamples == ce.offsetSamples,
                   "SessionDocument: offsetSamples (12345678) round-trips exactly", success);
            expect(lc.trimStartSamples == ce.trimStartSamples,
                   "SessionDocument: trimStartSamples round-trips exactly", success);
            expect(lc.trimEndSamples == ce.trimEndSamples,
                   "SessionDocument: trimEndSamples round-trips exactly", success);
            expect(lc.sourceLengthSamples == ce.sourceLengthSamples,
                   "SessionDocument: sourceLengthSamples round-trips exactly", success);
        }

        // Paths with special characters (backslash / forward slash mix).
        // On Linux paths are always '/'-separated; verify escaping does not corrupt.
        {
            ssbb::SessionData special;
            special.version = 1;
            ssbb::TakeEntry ste;
            ste.path         = (tmp.path / "take_with_quote\".wav").string();
            ste.sampleRate   = 48000.0;
            ste.numChannels  = 1;
            ste.isoTimestamp = "20260716T060000";
            special.takes.push_back(ste);

            const auto specialPath = tmp / "special.json";
            ssbb::SessionDocument::save(specialPath, special);

            ssbb::SessionData loadedSpecial;
            const bool okSpecial = ssbb::SessionDocument::load(specialPath, loadedSpecial);
            expect(okSpecial,
                   "SessionDocument: load() with quote in path returns true", success);
            if (okSpecial && !loadedSpecial.takes.empty())
                expect(loadedSpecial.takes[0].path == ste.path,
                       "SessionDocument: path with quote character round-trips", success);
        }

        // Empty session round-trip
        {
            ssbb::SessionData empty;
            empty.version = 1;
            const auto emptyPath = tmp / "empty.json";
            ssbb::SessionDocument::save(emptyPath, empty);

            ssbb::SessionData loadedEmpty;
            const bool okEmpty = ssbb::SessionDocument::load(emptyPath, loadedEmpty);
            expect(okEmpty,
                   "SessionDocument: empty session loads without error", success);
            expect(loadedEmpty.takes.empty(),
                   "SessionDocument: empty takes array round-trips", success);
            expect(loadedEmpty.clips.empty(),
                   "SessionDocument: empty clips array round-trips", success);
        }
    }

    // -------------------------------------------------------------------------
    // 4b. SessionDocument — saveRecovery / loadRecovery
    // -------------------------------------------------------------------------
    {
        TempDir tmp("ssbb_test_recovery");
        const auto sessionPath = tmp / "session.json";

        ssbb::SessionData orig;
        orig.version = 1;
        ssbb::TakeEntry te;
        te.path         = (tmp.path / "recovery_take.wav").string();
        te.sampleRate   = 44100.0;
        te.numChannels  = 1;
        te.isoTimestamp = "20260716T055400";
        orig.takes.push_back(te);

        // Save both main and recovery.
        const bool savedMain     = ssbb::SessionDocument::save(sessionPath, orig);
        const bool savedRecovery = ssbb::SessionDocument::saveRecovery(sessionPath, orig);
        expect(savedMain,     "SessionDocument: saveRecovery — main save ok",     success);
        expect(savedRecovery, "SessionDocument: saveRecovery() returns true",     success);

        // Recovery file must exist alongside the main file.
        const auto recoveryFile = tmp / "session.recovery.json";
        expect(std::filesystem::exists(recoveryFile),
               "SessionDocument: recovery file exists on disk", success);

        // Load recovery and verify it matches.
        ssbb::SessionData recovered;
        const bool ok = ssbb::SessionDocument::loadRecovery(sessionPath, recovered);
        expect(ok,
               "SessionDocument: loadRecovery() returns true", success);
        expect(recovered.version == 1,
               "SessionDocument: recovered version == 1", success);
        expect(recovered.takes.size() == 1,
               "SessionDocument: recovered takes.size() == 1", success);
        if (!recovered.takes.empty())
            expect(recovered.takes[0].path == te.path,
                   "SessionDocument: recovered take path matches", success);

        // loadRecovery must fail gracefully if the recovery file is absent.
        TempDir tmp2("ssbb_test_recovery_absent");
        const auto missing = tmp2 / "nonexistent.json";
        ssbb::SessionData dummy;
        const bool mustFail = ssbb::SessionDocument::loadRecovery(missing, dummy);
        expect(!mustFail,
               "SessionDocument: loadRecovery() returns false when file absent",
               success);
    }

    // -------------------------------------------------------------------------
    // 5. WaveformCache — initial state
    // -------------------------------------------------------------------------
    {
        ssbb::WaveformCache wc;

        expect(!wc.isReady(),
               "WaveformCache: isReady() starts false", success);
        expect(wc.getFrames().empty(),
               "WaveformCache: getFrames() starts empty", success);

        // Verify the Frame struct has the documented fields.
        ssbb::WaveformCache::Frame f{};
        f.peakPos = 0.9f;
        f.peakNeg = -0.9f;
        f.rms     = 0.6f;
        expect(f.peakPos == 0.9f,  "WaveformCache::Frame: peakPos field accessible", success);
        expect(f.peakNeg == -0.9f, "WaveformCache::Frame: peakNeg field accessible", success);
        expect(f.rms     == 0.6f,  "WaveformCache::Frame: rms field accessible",     success);

        // reset() keeps the cache in the not-ready state.
        wc.reset();
        expect(!wc.isReady(),
               "WaveformCache: isReady() still false after reset()", success);

        // buildFromFile on a non-existent path must not crash and must leave
        // isReady() false.
        wc.buildFromFile("/nonexistent/path/take.wav", 256);
        expect(!wc.isReady(),
               "WaveformCache: isReady() false after buildFromFile on missing path",
               success);
    }

    // =========================================================================
    // 6. PlaybackBuffer - load and render a completed take
    // -------------------------------------------------------------------------
    {
        TempDir tmp("ssbb_test_playback");
        const auto path = tmp / "take.wav";

        ssbb::WavWriter writer;
        expect(writer.open(path, 48000.0, 1),
               "PlaybackBuffer: test WAV opens", success);
        const float source[4] = { 0.1f, -0.2f, 0.3f, -0.4f };
        writer.write(source, 4);
        writer.close();

        ssbb::PlaybackBuffer playback;
        expect(playback.load(path), "PlaybackBuffer: loads recorded float WAV", success);
        expect(playback.isReady(), "PlaybackBuffer: ready after load", success);
        expect(playback.numFrames() == 4, "PlaybackBuffer: frame count", success);

        float left[4] = {};
        float right[4] = {};
        float* outputs[2] = { left, right };
        playback.render(outputs, 2, 4, 0, 48000.0);
        for (int i = 0; i < 4; ++i)
        {
            expect(left[i] == source[i], "PlaybackBuffer: left sample matches", success);
            expect(right[i] == source[i], "PlaybackBuffer: mono duplicates to right", success);
        }

        float mismatch[4] = {};
        float* mismatchOut[1] = { mismatch };
        playback.render(mismatchOut, 1, 4, 0, 44100.0);
        expect(mismatch[0] == 0.0f && mismatch[3] == 0.0f,
               "PlaybackBuffer: sample-rate mismatch fails silent", success);
    }

    // -------------------------------------------------------------------------
    // 7. VocalTrack - completed recording becomes playable
    // -------------------------------------------------------------------------
    {
        TempDir tmp("ssbb_test_vocal_playback");
        ssbb::VocalTrack track;
        track.prepare(44100.0, 64);
        track.setTakeDirectory(tmp.path);
        track.arm();
        track.startRecording();

        const float input[4] = { 0.25f, 0.5f, -0.25f, -0.5f };
        const float* inputs[1] = { input };
        float monitor[4] = {};
        float* monitorOutputs[1] = { monitor };
        track.processBlock(inputs, 1, monitorOutputs, 1, 4, 0, false);
        track.stopRecording();
        track.drainToFile();

        expect(track.getState() == ssbb::VocalTrack::State::Idle,
               "VocalTrack: returns to Idle after drain", success);
        expect(track.hasPlayback(),
               "VocalTrack: completed take is available for playback", success);

        float playbackOut[4] = {};
        float* playbackOutputs[1] = { playbackOut };
        track.processBlock(nullptr, 0, playbackOutputs, 1, 4, 0, true);
        for (int i = 0; i < 4; ++i)
            expect(playbackOut[i] == input[i],
                   "VocalTrack: latest take plays from transport zero", success);
    }

    // -------------------------------------------------------------------------
    // 8. VocalTrack - file-open failure never enters Recording
    // -------------------------------------------------------------------------
    {
        TempDir tmp("ssbb_test_record_open_failure");
        const auto blocker = tmp / "not_a_directory";
        {
            std::ofstream file(blocker);
            file << "block directory creation";
        }

        ssbb::VocalTrack track;
        track.prepare(44100.0, 64);
        track.setTakeDirectory(blocker);
        track.arm();
        track.startRecording();

        expect(track.getState() == ssbb::VocalTrack::State::Armed,
               "VocalTrack: failed file open leaves track Armed", success);
        expect(track.hasRecordingError(),
               "VocalTrack: failed file open exposes an error", success);
        expect(track.getTakeManager().takes().empty(),
               "VocalTrack: failed file open is not stored as a take", success);
    }

    // =========================================================================
    if (success)
        std::cout << "All vocal track tests passed.\n";
    else
        std::cerr << "One or more vocal track tests FAILED.\n";

    return success ? 0 : 1;
}
