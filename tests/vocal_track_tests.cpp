// vocal_track_tests.cpp
// Plain C++20 â€” NO JUCE headers, NO third-party test frameworks.
// Follows the same expect() / main() pattern as transport_tests.cpp.
//
// Links: VocalTrack.cpp, TakeManager.cpp, WaveformCache.cpp, SessionDocument.cpp
// Header-only: RecordBuffer.h, Clip.h, WavWriter.h
//
// Tested areas:
//   1. RecordBuffer â€” write/read round-trip and overflow drop
//   2. Clip         â€” trimStart / trimEnd / moveTo / activeLengthSamples
//   3. TakeManager  â€” unique timestamped path generation
//   4. SessionDocument â€” save/load/saveRecovery/loadRecovery JSON round-trip
//   5. WaveformCache   â€” initial state (isReady == false, frames empty)

#include "RecordBuffer.h"
#include "Clip.h"
#include "TakeManager.h"
#include "SessionDocument.h"
#include "WaveformCache.h"
#include "PlaybackBuffer.h"
#include "WavWriter.h"
#include "VocalTrack.h"

#include <cmath>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
        // Ignore error on cleanup â€” best-effort.
    }

    // Convenience: path to a file inside this directory.
    std::filesystem::path operator/(std::string_view name) const
    {
        return path / name;
    }
};

bool writePcm16Wav(const std::filesystem::path& path,
                   const std::array<int16_t, 4>& samples,
                   uint32_t sampleRate = 44100)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;

    auto write16 = [&file](uint16_t value)
    {
        const char bytes[2] = {
            static_cast<char>(value & 0xffu),
            static_cast<char>((value >> 8u) & 0xffu)
        };
        file.write(bytes, 2);
    };
    auto write32 = [&file](uint32_t value)
    {
        const char bytes[4] = {
            static_cast<char>(value & 0xffu),
            static_cast<char>((value >> 8u) & 0xffu),
            static_cast<char>((value >> 16u) & 0xffu),
            static_cast<char>((value >> 24u) & 0xffu)
        };
        file.write(bytes, 4);
    };

    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    file.write("RIFF", 4); write32(36u + dataBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4); write32(16u);
    write16(1u); write16(1u); write32(sampleRate);
    write32(sampleRate * 2u); write16(2u); write16(16u);
    file.write("data", 4); write32(dataBytes);
    file.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return file.good();
}

// ============================================================================
int main()
{
    bool success = true;

    // -------------------------------------------------------------------------
    // 1. RecordBuffer â€” write / read round-trip
    // -------------------------------------------------------------------------
    {
        // Use a small capacity for the test (power of 2, â‰¥ 2).
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
               "RecordBuffer: write 32 samples to capacity-64 â†’ true", success);
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
    // 1b. RecordBuffer â€” overflow / drop
    // -------------------------------------------------------------------------
    {
        ssbb::RecordBuffer<8> rb;
        float data[9] = {};

        // Fill the buffer completely.
        const bool ok1 = rb.write(data, 8);
        expect(ok1,
               "RecordBuffer: write 8 samples to capacity-8 â†’ true", success);

        // A further write of even 1 sample must be refused (returns false).
        const bool ok2 = rb.write(data, 1);
        expect(!ok2,
               "RecordBuffer: write to full capacity-8 buffer â†’ false (safe drop)", success);

        // The existing 8 samples must still be readable and correct (drop was
        // non-destructive).
        expect(rb.availableRead() == 8,
               "RecordBuffer: overflow drop does not corrupt existing data", success);
    }

    // -------------------------------------------------------------------------
    // 1c. RecordBuffer â€” zero-count edge cases
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
    // 2. Clip â€” non-destructive edits
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
    // 3. TakeManager â€” unique timestamped paths
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
    // 4. SessionDocument â€” JSON save / load round-trip
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

        // A second save replaces the document without leaving a stale backup.
        orig.clips[0].offsetSamples = 42;
        expect(ssbb::SessionDocument::save(sessionPath, orig),
               "SessionDocument: replacement save succeeds", success);
        ssbb::SessionData replaced;
        expect(ssbb::SessionDocument::load(sessionPath, replaced) &&
                   !replaced.clips.empty() && replaced.clips[0].offsetSamples == 42,
               "SessionDocument: replacement save loads newest data", success);
        expect(!std::filesystem::exists(sessionPath.string() + ".bak"),
               "SessionDocument: successful replacement removes backup", success);

        // Paths with special characters (backslash / ã»h‘éì¶»§q«^wÛ‘]HÜXÚX[ÂˆÜXÚX[™\œÚ[ÛˆHNÂˆÜØ˜ŽŽ•ZÙQ[žHÝNÂˆÝKœ]H
\œ]ÈZÙWÝÚ]Ü][ÝW‹Ø]ˆŠKœÝš[™Ê
NÂˆÝKœØ[\T˜]HHŒÂˆÝK›[PÚ[›™[ÈHNÂˆÝKš\ÛÕ[Y\Ý[\HŒŒŒÌM•ŒŽÂˆÜXÚX[ZÙ\Ëœ\ÚØ˜XÚÊÝJNÂ‚ˆÛÛœÝ]]ÈÜXÚX[]H\ÈœÜXÚX[šœÛÛˆŽÂˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[ŽœØ]™JÜXÚX[]ÜXÚX[
NÂ‚ˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘]HØYYÜXÚX[ÂˆÛÛœÝ›ÛÛÚÔÜXÚX[HÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[Ž›ØY
ÜXÚX[]ØYYÜXÚX[
NÂˆ^XÝ
ÚÔÜXÚX[ˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆØY

HÚ]][ÝH[ˆ]™]\›œÈYH‹ÝXØÙ\ÜÊNÂˆYˆ
ÚÔÜXÚX[	‰ˆ[ØYYÜXÚX[ZÙ\Ë™[\J
JBˆ^XÝ
ØYYÜXÚX[ZÙ\ÖÌKœ]OHÝKœ]ˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ]Ú]][ÝHÚ\˜XÝ\ˆ›Ý[™]š\È‹ÝXØÙ\ÜÊNÂˆB‚ˆËÈ[\HÙ\ÜÚ[Ûˆ›Ý[™]š\ˆÂˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘]H[\NÂˆ[\K™\œÚ[ÛˆHNÂˆÛÛœÝ]]È[\T]H\È™[\KšœÛÛˆŽÂˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[ŽœØ]™J[\T][\JNÂ‚ˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘]HØYY[\NÂˆÛÛœÝ›ÛÛÚÑ[\HHÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[Ž›ØY
[\T]ØYY[\JNÂˆ^XÝ
ÚÑ[\Kˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ[\HÙ\ÜÚ[ÛˆØYÈÚ]Ý]\œ›Üˆ‹ÝXØÙ\ÜÊNÂˆ^XÝ
ØYY[\KZÙ\Ë™[\J
Kˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ[\HZÙ\È\œ˜^H›Ý[™]š\È‹ÝXØÙ\ÜÊNÂˆ^XÝ
ØYY[\K˜Û\Ë™[\J
Kˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ[\HÛ\È\œ˜^H›Ý[™]š\È‹ÝXØÙ\ÜÊNÂˆBˆB‚ˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆËÈ‹ˆÙ\ÜÚ[Û‘ØÝ[Y[8 %Ø]™T™XÛÝ™\žHÈØY™XÛÝ™\žBˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆ[\\ˆ\
œÜØ˜—Ý\ÝÜ™XÛÝ™\žHŠNÂˆÛÛœÝ]]ÈÙ\ÜÚ[Û”]H\ÈœÙ\ÜÚ[Û‹šœÛÛˆŽÂ‚ˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘]HÜšYÎÂˆÜšYË™\œÚ[ÛˆHNÂˆÜØ˜ŽŽ•ZÙQ[žHNÂˆKœ]H
\œ]Èœ™XÛÝ™\žWÝZÙKØ]ˆŠKœÝš[™Ê
NÂˆKœØ[\T˜]HHLŒÂˆK›[PÚ[›™[ÈHNÂˆKš\ÛÕ[Y\Ý[\HŒŒŒÌM•MMŽÂˆÜšYËZÙ\Ëœ\ÚØ˜XÚÊJNÂ‚ˆËÈØ]™H›ÝXZ[ˆ[™™XÛÝ™\žK‚ˆÛÛœÝ›ÛÛØ]™YXZ[ˆHÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[ŽœØ]™JÙ\ÜÚ[Û”]ÜšYÊNÂˆÛÛœÝ›ÛÛØ]™Y™XÛÝ™\žHHÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[ŽœØ]™T™XÛÝ™\žJÙ\ÜÚ[Û”]ÜšYÊNÂˆ^XÝ
Ø]™YXZ[‹”Ù\ÜÚ[Û‘ØÝ[Y[ˆØ]™T™XÛÝ™\žH8 %XZ[ˆØ]™HÚÈ‹ÝXØÙ\ÜÊNÂˆ^XÝ
Ø]™Y™XÛÝ™\žK”Ù\ÜÚ[Û‘ØÝ[Y[ˆØ]™T™XÛÝ™\žJ
H™]\›œÈYH‹ÝXØÙ\ÜÊNÂ‚ˆËÈ™XÛÝ™\žHš[H]\Ý^\Ý[Û™ÜÚYHHXZ[ˆš[K‚ˆÛÛœÝ]]È™XÛÝ™\žQš[HH\ÈœÙ\ÜÚ[Û‹œ™XÛÝ™\žKšœÛÛˆŽÂˆ^XÝ
ÝŽ™š[\Þ\Ý[NŽ™^\ÝÊ™XÛÝ™\žQš[JKˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ™XÛÝ™\žHš[H^\ÝÈÛˆ\ÚÈ‹ÝXØÙ\ÜÊNÂ‚ˆËÈØY™XÛÝ™\žH[™™\šYžH]X]Ú\Ë‚ˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘]H™XÛÝ™\™YÂˆÛÛœÝ›ÛÛÚÈHÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[Ž›ØY™XÛÝ™\žJÙ\ÜÚ[Û”]™XÛÝ™\™Y
NÂˆ^XÝ
ÚËˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆØY™XÛÝ™\žJ
H™]\›œÈYH‹ÝXØÙ\ÜÊNÂˆ^XÝ
™XÛÝ™\™Y™\œÚ[ÛˆOHKˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ™XÛÝ™\™Y™\œÚ[ÛˆOHH‹ÝXØÙ\ÜÊNÂˆ^XÝ
™XÛÝ™\™YZÙ\ËœÚ^™J
HOHKˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ™XÛÝ™\™YZÙ\ËœÚ^™J
HOHH‹ÝXØÙ\ÜÊNÂˆYˆ
\™XÛÝ™\™YZÙ\Ë™[\J
JBˆ^XÝ
™XÛÝ™\™YZÙ\ÖÌKœ]OHKœ]ˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆ™XÛÝ™\™YZÙH]X]Ú\È‹ÝXØÙ\ÜÊNÂ‚ˆËÈØY™XÛÝ™\žH]\Ý˜Z[Ü˜XÙY[HYˆH™XÛÝ™\žHš[H\ÈXœÙ[‚ˆ[\\ˆ\ŠœÜØ˜—Ý\ÝÜ™XÛÝ™\žWØXœÙ[ŠNÂˆÛÛœÝ]]ÈZ\ÜÚ[™ÈH\ˆÈ››Û™^\Ý[šœÛÛˆŽÂˆÜØ˜ŽŽ”Ù\ÜÚ[Û‘]H[[^NÂˆÛÛœÝ›ÛÛ]\Ý˜Z[HÜØ˜ŽŽ”Ù\ÜÚ[Û‘ØÝ[Y[Ž›ØY™XÛÝ™\žJZ\ÜÚ[™Ë[[^JNÂˆ^XÝ
[]\Ý˜Z[ˆ”Ù\ÜÚ[Û‘ØÝ[Y[ˆØY™XÛÝ™\žJ
H™]\›œÈ˜[ÙHÚ[ˆš[HXœÙ[‹ˆÝXØÙ\ÜÊNÂˆB‚ˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆËÈKˆØ]™Y›Ü›PØXÚH8 %[š]X[Ý]BˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆÜØ˜ŽŽ•Ø]™Y›Ü›PØXÚHØÎÂ‚ˆ^XÝ
]ØËš\Ô™XYJ
Kˆ•Ø]™Y›Ü›PØXÚNˆ\Ô™XYJ
HÝ\È˜[ÙH‹ÝXØÙ\ÜÊNÂˆ^XÝ
ØË™Ù]œ˜[Y\Ê
K™[\J
Kˆ•Ø]™Y›Ü›PØXÚNˆÙ]œ˜[Y\Ê
HÝ\È[\H‹ÝXØÙ\ÜÊNÂ‚ˆËÈ™\šYžHHœ˜[YHÝXÝ\ÈHØÝ[Y[YšY[Ë‚ˆÜØ˜ŽŽ•Ø]™Y›Ü›PØXÚNŽ‘œ˜[YHžßNÂˆ‹œXZÔÜÈHŽYŽÂˆ‹œXZÓ™YÈHLŽYŽÂˆ‹œ›\ÈH™ŽÂˆ^XÝ
‹œXZÔÜÈOHŽY‹•Ø]™Y›Ü›PØXÚNŽ‘œ˜[YNˆXZÔÜÈšY[XØÙ\ÜÚX›H‹ÝXØÙ\ÜÊNÂˆ^XÝ
‹œXZÓ™YÈOHLŽY‹•Ø]™Y›Ü›PØXÚNŽ‘œ˜[YNˆXZÓ™YÈšY[XØÙ\ÜÚX›H‹ÝXØÙ\ÜÊNÂˆ^XÝ
‹œ›\ÈOH™‹•Ø]™Y›Ü›PØXÚNŽ‘œ˜[YNˆ›\ÈšY[XØÙ\ÜÚX›H‹ÝXØÙ\ÜÊNÂ‚ˆËÈ™\Ù]

HÙY\ÈHØXÚH[ˆH›Ý\™XYHÝ]K‚ˆØËœ™\Ù]

NÂˆ^XÝ
]ØËš\Ô™XYJ
Kˆ•Ø]™Y›Ü›PØXÚNˆ\Ô™XYJ
HÝ[˜[ÙHY\ˆ™\Ù]

H‹ÝXØÙ\ÜÊNÂ‚ˆËÈZ[œ›ÛQš[HÛˆH›Û‹Y^\Ý[]]\Ý›ÝÜ˜\Ú[™]\ÝX]™BˆËÈ\Ô™XYJ
H˜[ÙK‚ˆØË˜Z[œ›ÛQš[J‹Û›Û™^\Ý[Ü]ÝZÙKØ]ˆ‹MŠNÂˆ^XÝ
]ØËš\Ô™XYJ
Kˆ•Ø]™Y›Ü›PØXÚNˆ\Ô™XYJ
H˜[ÙHY\ˆZ[œ›ÛQš[HÛˆZ\ÜÚ[™È]‹ˆÝXØÙ\ÜÊNÂˆB‚ˆËÈOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOBˆËÈ‹ˆ^X˜XÚÐY™™\ˆHØY[™™[™\ˆHÛÛ\]YZÙBˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆ[\\ˆ\
œÜØ˜—Ý\ÝÜ^X˜XÚÈŠNÂˆÛÛœÝ]]È]H\ÈZÙKØ]ˆŽÂ‚ˆÜØ˜ŽŽ•Ø]•Üš]\ˆÜš]\ŽÂˆ^XÝ
Üš]\‹›Ü[Š]ŒJKˆ”^X˜XÚÐY™™\Žˆ\ÝÐUˆÜ[œÈ‹ÝXØÙ\ÜÊNÂˆÛÛœÝ›Ø]ÛÝ\˜ÙVÍHHÈŒY‹LŒ™‹ŒÙ‹LˆNÂˆÜš]\‹Üš]JÛÝ\˜ÙK
NÂˆÜš]\‹˜ÛÜÙJ
NÂ‚ˆÜØ˜ŽŽ”^X˜XÚÐY™™\ˆ^X˜XÚÎÂˆ^XÝ
^X˜XÚË›ØY
]
K”^X˜XÚÐY™™\ŽˆØYÈ™XÛÜ™Y›Ø]ÐUˆ‹ÝXØÙ\ÜÊNÂˆ^XÝ
^X˜XÚËš\Ô™XYJ
K”^X˜XÚÐY™™\Žˆ™XYHY\ˆØY‹ÝXØÙ\ÜÊNÂˆ^XÝ
^X˜XÚË›[Qœ˜[Y\Ê
HOH”^X˜XÚÐY™™\Žˆœ˜[YHÛÝ[‹ÝXØÙ\ÜÊNÂ‚ˆ›Ø]YÍHHßNÂˆ›Ø]šYÚÍHHßNÂˆ›Ø]
ˆÝ]]ÖÌ—HHÈYšYÚNÂˆ^X˜XÚËœ™[™\ŠÝ]]Ë‹Œ
NÂˆ›Üˆ
[HHÈHÈ
ÊÚJBˆÂˆ^XÝ
YÚWHOHÛÝ\˜ÙVÚWK”^X˜XÚÐY™™\ŽˆYØ[\HX]Ú\È‹ÝXØÙ\ÜÊNÂˆ^XÝ
šYÚÚWHOHÛÝ\˜ÙVÚWK”^X˜XÚÐY™™\Žˆ[Û›È\XØ]\ÈÈšYÚ‹ÝXØÙ\ÜÊNÂˆB‚ˆ›Ø]Y]YÍ×HHßNÂˆ›Ø]
ˆY]YÝ]ÌWHHÈY]YNÂˆ^X˜XÚËœ™[™\Û\
Y]YÝ]KËŒˆÊ˜Û\Ù™œÙ]Ø[\\ÏJ‹Ì‹ˆÊš[TÝ\Ø[\\ÏJ‹ÌKˆÊš[Q[™Ø[\\ÏJ‹ÌJNÂˆ^XÝ
Y]YÌHOHŒˆ	‰ˆY]YÌWHOHŒ‹ˆ”^X˜XÚÐY™™\Žˆ[Ý™YÛ\\ÈÚ[[™Y›Ü™H]ÈÙ™œÙ]‹ÝXØÙ\ÜÊNÂˆ^XÝ
Y]YÌ—HOHÛÝ\˜ÙVÌWH	‰ˆY]YÌ×HOHÛÝ\˜ÙVÌ—Kˆ”^X˜XÚÐY™™\Žˆš[H[™[Ý™H\™H›Û‹Y\ÝXÝ]™H‹ÝXØÙ\ÜÊNÂˆ^XÝ
Y]YÍHOHŒˆ	‰ˆY]YÍ—HOHŒ‹ˆ”^X˜XÚÐY™™\Žˆš[[YYÛ\[™È]Y]Y›Ý[™\žH‹ÝXØÙ\ÜÊNÂ‚ˆ›Ø]Z\ÛX]ÚÍHHßNÂˆ›Ø]
ˆZ\ÛX]ÚÝ]ÌWHHÈZ\ÛX]ÚNÂˆ^X˜XÚËœ™[™\ŠZ\ÛX]ÚÝ]KLŒ
NÂˆ^XÝ
Z\ÛX]ÚÌHOHŒˆ	‰ˆZ\ÛX]ÚÌ×HOHŒ‹ˆ”^X˜XÚÐY™™\ŽˆØ[\K\˜]HZ\ÛX]Ú˜Z[ÈÚ[[‹ÝXØÙ\ÜÊNÂˆB‚ˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆËÈËˆ›ØØ[˜XÚÈHÛÛ\]Y™XÛÜ™[™È™XÛÛY\È^XX›BˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆ[\\ˆ\
œÜØ˜—Ý\ÝÝ›ØØ[Ü^X˜XÚÈŠNÂˆ]]È˜XÚÈHÝŽ›XZÙWÝ[š\]YOÜØ˜ŽŽ•›ØØ[˜XÚÏŠ
NÂˆ˜XÚËOœ™\\™JLŒ
NÂˆ˜XÚËOœÙ]ZÙQ\™XÝÜžJ\œ]
NÂˆ˜XÚËO˜\›J
NÂˆ˜XÚËOœÝ\™XÛÜ™[™Ê
NÂ‚ˆÛÛœÝ›Ø][œ]ÍHHÈŒY‹Y‹LŒY‹LYˆNÂˆÛÛœÝ›Ø]
ˆ[œ]ÖÌWHHÈ[œ]NÂˆ›Ø][Ûš]Ü–ÍHHßNÂˆ›Ø]
ˆ[Ûš]Ü“Ý]]ÖÌWHHÈ[Ûš]ÜˆNÂˆ˜XÚËOœ›ØÙ\ÜÐ›ØÚÊ[œ]ËK[Ûš]Ü“Ý]]ËK˜[ÙJNÂˆ˜XÚËOœÝÜ™XÛÜ™[™Ê
NÂˆ˜XÚËO™˜Z[•Ñš[J
NÂ‚ˆ^XÝ
˜XÚËO™Ù]Ý]J
HOHÜØ˜ŽŽ•›ØØ[˜XÚÎŽ”Ý]NŽ’YKˆ•›ØØ[˜XÚÎˆ™]\›œÈÈYHY\ˆ˜Z[ˆ‹ÝXØÙ\ÜÊNÂˆ^XÝ
˜XÚËOš\Ô^X˜XÚÊ
Kˆ•›ØØ[˜XÚÎˆÛÛ\]YZÙH\È]˜Z[X›H›Üˆ^X˜XÚÈ‹ÝXØÙ\ÜÊNÂ‚ˆ›Ø]^X˜XÚÓÝ]ÍHHßNÂˆ›Ø]
ˆ^X˜XÚÓÝ]]ÖÌWHHÈ^X˜XÚÓÝ]NÂˆ˜XÚËOœ›ØÙ\ÜÐ›ØÚÊ[‹^X˜XÚÓÝ]]ËKYJNÂˆ›Üˆ
[HHÈHÈ
ÊÚJBˆ^XÝ
^X˜XÚÓÝ]ÚWHOH[œ]ÚWKˆ•›ØØ[˜XÚÎˆ]\ÝZÙH^\Èœ›ÛH˜[œÜÜ™\›È‹ÝXØÙ\ÜÊNÂˆB‚ˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆËÈˆ›ØØ[˜XÚÈHš[K[Ü[ˆ˜Z[\™H™]™\ˆ[\œÈ™XÛÜ™[™ÂˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆ[\\ˆ\
œÜØ˜—Ý\ÝÜ™XÛÜ™ÛÜ[—Ù˜Z[\™HŠNÂˆÛÛœÝ]]È›ØÚÙ\ˆH\È››ÝØWÙ\™XÝÜžHŽÂˆÂˆÝŽ›ÙœÝ™X[Hš[J›ØÚÙ\ŠNÂˆš[H˜›ØÚÈ\™XÝÜžHÜ™X][ÛˆŽÂˆB‚ˆ]]È˜XÚÈHÝŽ›XZÙWÝ[š\]YOÜØ˜ŽŽ•›ØØ[˜XÚÏŠ
NÂˆ˜XÚËOœ™\\™JLŒ
NÂˆ˜XÚËOœÙ]ZÙQ\™XÝÜžJ›ØÚÙ\ŠNÂˆ˜XÚËO˜\›J
NÂˆ˜XÚËOœÝ\™XÛÜ™[™Ê
NÂ‚ˆ^XÝ
˜XÚËO™Ù]Ý]J
HOHÜØ˜ŽŽ•›ØØ[˜XÚÎŽ”Ý]NŽ\›YYˆ•›ØØ[˜XÚÎˆ˜Z[Yš[HÜ[ˆX]™\È˜XÚÈ\›YY‹ÝXØÙ\ÜÊNÂˆ^XÝ
˜XÚËOš\Ô™XÛÜ™[™Ñ\œ›ÜŠ
Kˆ•›ØØ[˜XÚÎˆ˜Z[Yš[HÜ[ˆ^ÜÙ\È[ˆ\œ›Üˆ‹ÝXØÙ\ÜÊNÂˆ^XÝ
˜XÚËO™Ù]ZÙSX[˜YÙ\Š
KZÙ\Ê
K™[\J
Kˆ•›ØØ[˜XÚÎˆ˜Z[Yš[HÜ[ˆ\È›ÝÝÜ™Y\ÈHZÙH‹ÝXØÙ\ÜÊNÂˆB‚ˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆËÈKˆÓLMˆ[\Ü[™›Ø]ÐUˆ^ÜˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆ[\\ˆ\
œÜØ˜—Ý\ÝÜÛWÚ[\ÜÙ^ÜŠNÂˆÛÛœÝ]]È[œ]]H\Èš[œ]M‹Ø]ˆŽÂˆÛÛœÝÝŽ˜\œ˜^O[M—ÝˆÛHÈMŒÎLMŒÎÌÍÈNÂˆ^XÝ
Üš]TÛLM•Ø]Š[œ]]ÛJKˆ”^X˜XÚÐY™™\ŽˆÓLMˆš^\™HÜš][ˆ‹ÝXØÙ\ÜÊNÂ‚ˆÜØ˜ŽŽ”^X˜XÚÐY™™\ˆ^X˜XÚÎÂˆ^XÝ
^X˜XÚË›ØY
[œ]]
Kˆ”^X˜XÚÐY™™\ŽˆØYÈÓLMˆÐUˆ‹ÝXØÙ\ÜÊNÂˆ›Ø]Ý]]ÍHHßNÂˆ›Ø]
ˆÚ[›™[ÖÌWHHÈÝ]]NÂˆ^X˜XÚËœ™[™\ŠÚ[›™[ËKLŒ
NÂˆ^XÝ
ÝŽ˜XœÊÝ]]ÌWHHYŠHŒY‹ˆ”^X˜XÚÐY™™\ŽˆÓLMˆÜÚ]]™HØ[\HÛÛ™\È‹ÝXØÙ\ÜÊNÂˆ^XÝ
ÝŽ˜XœÊÝ]]Ì—H
ÈYŠHŒY‹ˆ”^X˜XÚÐY™™\ŽˆÓLMˆ™YØ]]™HØ[\HÛÛ™\È‹ÝXØÙ\ÜÊNÂ‚ˆÛÛœÝ]]È^Ü]H\È™^ÜØ]ˆŽÂˆ^XÝ
^X˜XÚË™^ÜÊ^Ü]
Kˆ”^X˜XÚÐY™™\Žˆ^ÜÈÝ\œ™[]Y[È‹ÝXØÙ\ÜÊNÂˆÜØ˜ŽŽ”^X˜XÚÐY™™\ˆ^ÜYÂˆ^XÝ
^ÜY›ØY
^Ü]
Kˆ”^X˜XÚÐY™™\Žˆ^ÜYÐUˆ™[ØYÈ‹ÝXØÙ\ÜÊNÂˆ^XÝ
^ÜY›[Qœ˜[Y\Ê
HOHˆ”^X˜XÚÐY™™\Žˆ^ÜYœ˜[YHÛÝ[‹ÝXØÙ\ÜÊNÂˆB‚ˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆËÈLˆÛÜšÙ\ˆ[\ÜØ]™Y›Ü›K]]ÜØ]™K^Ü[™™XÛÝ™\žBˆËÈKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKBˆÂˆ[\\ˆ\
œÜØ˜—Ý\ÝÝÛÜšÙ\—Ú›ØœÈŠNÂˆÛÛœÝ]]ÈÛÝ\˜ÙT]H\È›ÝÛ™YØ]ˆŽÂˆÜØ˜ŽŽ•Ø]•Üš]\ˆÜš]\ŽÂˆ^XÝ
Üš]\‹›Ü[ŠÛÝ\˜ÙT]LŒJKˆ•ÛÜšÙ\ˆ›ØœÎˆÛÝ\˜ÙHÜ[œÈ‹ÝXØÙ\ÜÊNÂˆÝŽ˜\œ˜^O›Ø]LLˆØ[\\ÈßNÂˆ›Üˆ
ÝŽœÚ^™WÝHHÈHØ[\\ËœÚ^™J
NÈ
ÊÚJBˆØ[\\ÖÚWHH
H	HˆOH
HÈŒYˆˆLŒYŽÂˆ^XÝ
Üš]\‹Üš]JØ[\\Ë™]J
KÝ]X×ØØ\Ý[ŠØ[\\ËœÚ^™J
JJKˆ•ÛÜšÙ\ˆ›ØœÎˆÛÝ\˜ÙHÜš]\È‹ÝXØÙ\ÜÊNÂˆÜš]\‹˜ÛÜÙJ
NÂ‚ˆ]]È˜XÚÈHÝŽ›XZÙWÝ[š\]YOÜØ˜ŽŽ•›ØØ[˜XÚÏŠ
NÂˆ˜XÚËOœ™\\™JLŒ
NÂˆ˜XÚËOœÙ]ZÙQ\™XÝÜžJ\œ]
NÂˆ˜XÚËOœ™\]Y\Ý[\Ü
ÛÝ\˜ÙT]
NÂˆ˜XÚËOœÙ\šXÙUÛÜšÙ\•\ÚÜÊ
NÂˆ^XÝ
˜XÚËO™Ù]ÛÜšÙ\”Ý]\Ê
HOHÜØ˜ŽŽ•›ØØ[˜XÚÎŽ•ÛÜšÙ\”Ý]\ÎŽ’[\ÜÝXØÙYYYˆ•ÛÜšÙ\ˆ›ØœÎˆ[\ÜÝXØÙYYÈ‹ÝXØÙ\ÜÊNÂˆ^XÝ
˜XÚËOš\Ô^X˜XÚÊ
K•ÛÜšÙ\ˆ›ØœÎˆ[\Ü™XÛÛY\È^XX›H‹ÝXØÙ\ÜÊNÂ‚ˆÝŽ™XÝÜÜØ˜ŽŽ•Ø]™Y›Ü›PØXÚNŽ‘œ˜[YOˆØ]™Y›Ü›NÂˆZ[ÝÙ[™\˜][ÛˆHÂˆ^XÝ
˜XÚËO˜ÛÜUØ]™Y›Ü›RYÚ[™ÙY
Ø]™Y›Ü›KÙ[™\˜][ÛŠKˆ•ÛÜšÙ\ˆ›ØœÎˆØ]™Y›Ü›HÛ˜\ÚÝX›\ÚY‹ÝXØÙ\ÜÊNÂˆ^XÝ
]Ø]™Y›Ü›K™[\J
K•ÛÜšÙ\ˆ›ØœÎˆØ]™Y›Ü›HÛÛZ[œÈœ˜[Y\È‹ÝXØÙ\ÜÊNÂ‚ˆ˜XÚËOœÙ]Û\Ù™œÙ]Ø[\\ÊÌŠNÂˆ˜XÚËOœÙ]š[TÝ\Ø[\\ÊJNÂˆ˜XÚËOœÙ]š[Q[™Ø[\\ÊÊNÂˆ˜XÚËOœÙ\šXÙUÛÜšÙ\•\ÚÜÊ
NÂˆ^XÝ
˜XÚËO™Ù]ÛÜšÙ\”Ý]\Ê
HOHÜØ˜ŽŽ•›ØØ[˜XÚÎŽ•ÛÜšÙ\”Ý]\ÎŽ]]ÜØ]™TÝXØÙYYYˆ•ÛÜšÙ\ˆ›ØœÎˆY™\œ™Y]]ÜØ]™HÝXØÙYYÈ‹ÝXØÙ\ÜÊNÂˆ^XÝ
˜XÚËOœ™XÛÝ™\žP]˜Z[X›J
Kˆ•ÛÜšÙ\ˆ›ØœÎˆ™XÛÝ™\žHY]Y]H^\ÝÈ‹ÝXØÙ\ÜÊNÂ‚ˆÛÛœÝ]]È^Ü]H\ÈÛÜšÙ\‹Y^ÜØ]ˆŽÂˆ˜XÚËOœ™\]Y\Ý^Ü
^Ü]
NÂˆ˜XÚËOœÙ\šXÙUÛÜšÙ\•\ÚÜÊ
NÂˆ^XÝ
˜XÚËO™Ù]ÛÜšÙ\”Ý]\Ê
HOHÜØ˜ŽŽ•›ØØ[˜XÚÎŽ•ÛÜšÙ\”Ý]\ÎŽ‘^ÜÝXØÙYYYˆ•ÛÜšÙ\ˆ›ØœÎˆ^ÜÝXØÙYYÈ‹ÝXØÙ\ÜÊNÂˆ^XÝ
ÝŽ™š[\Þ\Ý[NŽ™^\ÝÊ^Ü]
Kˆ•ÛÜšÙ\ˆ›ØœÎˆ^Üš[H^\ÝÈ‹ÝXØÙ\ÜÊNÂ‚ˆ]]È™XÛÝ™\™YHÝŽ›XZÙWÝ[š\]YOÜØ˜ŽŽ•›ØØ[˜XÚÏŠ
NÂˆ™XÛÝ™\™YOœ™\\™JLŒ
NÂˆ™XÛÝ™\™YOœÙ]ZÙQ\™XÝÜžJ\œ]
NÂˆ™XÛÝ™\™YOœ™\]Y\Ý™XÛÝ™\žSØY

NÂˆ™XÛÝ™\™YOœÙ\šXÙUÛÜšÙ\•\ÚÜÊ
NÂˆ^XÝ
™XÛÝ™\™YO™Ù]ÛÜšÙ\”Ý]\Ê
HOBˆÜØ˜ŽŽ•›ØØ[˜XÚÎŽ•ÛÜšÙ\”Ý]\ÎŽ”™XÛÝ™\žTÝXØÙYYYˆ•ÛÜšÙ\ˆ›ØœÎˆ™XÛÝ™\žHÝXØÙYYÈ‹ÝXØÙ\ÜÊNÂˆ^XÝ
™XÛÝ™\™YOš\Ô^X˜XÚÊ
Kˆ•ÛÜšÙ\ˆ›ØœÎˆ™XÛÝ™\™YÛÝ\˜ÙH\È^XX›H‹ÝXØÙ\ÜÊNÂˆ^XÝ
™XÛÝ™\™YO™Ù]Û\Ù™œÙ]Ø[\\Ê
HOHÌˆ	‰‚ˆ™XÛÝ™\™YO™Ù]š[TÝ\Ø[\\Ê
HOHH	‰‚ˆ™XÛÝ™\™YO™Ù]š[Q[™Ø[\\Ê
HOHËˆ•ÛÜšÙ\ˆ›ØœÎˆ™XÛÝ™\žH™\ÝÜ™\È›Û‹Y\ÝXÝ]™HY]È‹ÝXØÙ\ÜÊNÂˆB‚ˆËÈOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOBˆYˆ
ÝXØÙ\ÜÊBˆÝŽ˜ÛÝ][›ØØ[˜XÚÈ\ÝÈ\ÜÙY—ˆŽÂˆ[ÙBˆÝŽ˜Ù\œˆ“Û™HÜˆ[Ü™H›ØØ[˜XÚÈ\ÝÈRSQ—ˆŽÂ‚ˆ™]\›ˆÝXØÙ\ÜÈÈˆNÂŸB