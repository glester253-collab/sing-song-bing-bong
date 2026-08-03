// playback_tests.cpp
// Plain C++20 — NO JUCE headers, NO third-party test frameworks.
// Follows the same expect() / main() pattern as transport_tests.cpp.
//
// Tested areas:
//   1. WavWriter + WavReader — IEEE float round-trip (mono and stereo)
//   2. WavReader             — PCM 16-bit import
//   3. WavReader             — rejects unknown format / bad file
//   4. ClipPlayer            — load, play, processBlock, auto-stop
//   5. CommandHistory        — execute, undo, redo, depth limit
//   6. SessionDocument::migrate — known and unknown version

#include "WavWriter.h"
#include "WavReader.h"
#include "ClipPlayer.h"
#include "CommandHistory.h"
#include "SessionDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

constexpr float kEpsF = 1e-5f;

bool nearEqF(float a, float b)
{
    return std::abs(a - b) < kEpsF;
}

// ---- Helpers ----------------------------------------------------------------

/// Create a temporary directory under the system temp path.
std::filesystem::path makeTempDir(std::string_view name)
{
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(dir);
    return dir;
}

/// Write a minimal IEEE float WAV to `path` with the given interleaved samples.
bool writeFloatWav(const std::filesystem::path& path,
                   const std::vector<float>& samples,
                   double sampleRate, int numChannels)
{
    ssbb::WavWriter w;
    if (!w.open(path, sampleRate, numChannels)) return false;
    w.write(samples.data(), static_cast<int>(samples.size()));
    w.close();
    return true;
}

/// Write a PCM-16 WAV manually (WavWriter only writes float-32).
bool writePcm16Wav(const std::filesystem::path& path,
                   const std::vector<int16_t>& samples,
                   uint32_t sampleRate, uint16_t numChannels)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;

    const uint32_t dataBytes  = static_cast<uint32_t>(samples.size()) * 2u;
    const uint32_t byteRate   = sampleRate * numChannels * 2u;
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * 2);

    auto w16 = [&](uint16_t v) {
        uint8_t b[2] = { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8u) };
        f.write(reinterpret_cast<const char*>(b), 2);
    };
    auto w32 = [&](uint32_t v) {
        uint8_t b[4] = { static_cast<uint8_t>(v),
                         static_cast<uint8_t>(v >> 8u),
                         static_cast<uint8_t>(v >> 16u),
                         static_cast<uint8_t>(v >> 24u) };
        f.write(reinterpret_cast<const char*>(b), 4);
    };

    f.write("RIFF", 4);  w32(36u + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);  w32(16u);
    w16(1u);             // PCM
    w16(numChannels);
    w32(sampleRate);
    w32(byteRate);
    w16(blockAlign);
    w16(16u);            // bits per sample
    f.write("data", 4);  w32(dataBytes);
    f.write(reinterpret_cast<const char*>(samples.data()),
            static_cast<std::streamsize>(dataBytes));
    return f.good();
}

// ---- Test: WavWriter + WavReader round-trip (mono) --------------------------

bool testWavRoundTripMono()
{
    bool ok = true;
    const auto dir = makeTempDir("ssbb_playback_tests");
    const auto path = dir / "mono.wav";

    // Build a simple ramp signal (512 mono samples).
    std::vector<float> original(512);
    for (int i = 0; i < 512; ++i)
        original[i] = static_cast<float>(i) / 512.0f - 0.5f;

    expect(writeFloatWav(path, original, 44100.0, 1),
           "WavRoundTripMono: write failed", ok);

    ssbb::WavReader reader;
    expect(reader.open(path), "WavRoundTripMono: read failed", ok);
    expect(reader.numChannels() == 1, "WavRoundTripMono: numChannels", ok);
    expect(reader.sampleRate() == 44100.0, "WavRoundTripMono: sampleRate", ok);
    expect(reader.numFrames() == 512, "WavRoundTripMono: numFrames", ok);
    expect(reader.samples().size() == 512, "WavRoundTripMono: samples size", ok);

    bool samplesMatch = true;
    for (int i = 0; i < 512; ++i)
    {
        if (!nearEqF(reader.samples()[static_cast<std::size_t>(i)], original[static_cast<std::size_t>(i)]))
        {
            samplesMatch = false;
            break;
        }
    }
    expect(samplesMatch, "WavRoundTripMono: sample values", ok);

    std::filesystem::remove(path);
    return ok;
}

// ---- Test: WavWriter + WavReader round-trip (stereo) -----------------------

bool testWavRoundTripStereo()
{
    bool ok = true;
    const auto dir = makeTempDir("ssbb_playback_tests");
    const auto path = dir / "stereo.wav";

    // 256 frames of stereo (512 interleaved samples).
    std::vector<float> original(512);
    for (int i = 0; i < 512; ++i)
        original[static_cast<std::size_t>(i)] = static_cast<float>(i) / 512.0f;

    expect(writeFloatWav(path, original, 48000.0, 2),
           "WavRoundTripStereo: write failed", ok);

    ssbb::WavReader reader;
    expect(reader.open(path), "WavRoundTripStereo: read failed", ok);
    expect(reader.numChannels() == 2, "WavRoundTripStereo: numChannels", ok);
    expect(reader.sampleRate() == 48000.0, "WavRoundTripStereo: sampleRate", ok);
    expect(reader.numFrames() == 256, "WavRoundTripStereo: numFrames", ok);

    bool samplesMatch = true;
    const auto& s = reader.samples();
    for (std::size_t i = 0; i < 512; ++i)
    {
        if (!nearEqF(s[i], original[i]))
        {
            samplesMatch = false;
            break;
        }
    }
    expect(samplesMatch, "WavRoundTripStereo: sample values", ok);

    std::filesystem::remove(path);
    return ok;
}

// ---- Test: WavReader PCM 16-bit import --------------------------------------

bool testWavReaderPcm16()
{
    bool ok = true;
    const auto dir = makeTempDir("ssbb_playback_tests");
    const auto path = dir / "pcm16.wav";

    // 4 samples: 0, 16384, -16384, 32767
    std::vector<int16_t> raw = { 0, 16384, -16384, 32767 };
    expect(writePcm16Wav(path, raw, 44100u, 1u),
           "WavReaderPcm16: write failed", ok);

    ssbb::WavReader reader;
    expect(reader.open(path), "WavReaderPcm16: read failed", ok);
    expect(reader.numChannels() == 1, "WavReaderPcm16: numChannels", ok);
    expect(reader.numFrames() == 4, "WavReaderPcm16: numFrames", ok);

    constexpr float kScale = 1.0f / 32768.0f;
    const auto& s = reader.samples();
    expect(nearEqF(s[0],  0.0f),                      "WavReaderPcm16: s[0]", ok);
    expect(nearEqF(s[1],  16384.0f * kScale),          "WavReaderPcm16: s[1]", ok);
    expect(nearEqF(s[2], -16384.0f * kScale),          "WavReaderPcm16: s[2]", ok);
    expect(nearEqF(s[3],  32767.0f * kScale),          "WavReaderPcm16: s[3]", ok);

    std::filesystem::remove(path);
    return ok;
}

// ---- Test: WavReader rejects bad files --------------------------------------

bool testWavReaderRejectsBadFile()
{
    bool ok = true;
    const auto dir = makeTempDir("ssbb_playback_tests");

    // Non-existent file
    ssbb::WavReader r1;
    expect(!r1.open(dir / "nonexistent.wav"),
           "WavReaderRejectsBad: nonexistent", ok);

    // Empty file
    const auto emptyPath = dir / "empty.wav";
    { std::ofstream f(emptyPath); }
    ssbb::WavReader r2;
    expect(!r2.open(emptyPath),
           "WavReaderRejectsBad: empty", ok);

    // Wrong magic
    const auto badPath = dir / "bad.wav";
    {
        std::ofstream f(badPath, std::ios::binary);
        f.write("JUNK\x00\x00\x00\x00WAVE", 12);
    }
    ssbb::WavReader r3;
    expect(!r3.open(badPath),
           "WavReaderRejectsBad: bad magic", ok);

    std::filesystem::remove(emptyPath);
    std::filesystem::remove(badPath);
    return ok;
}

// ---- Test: ClipPlayer load, play, processBlock, auto-stop ------------------

bool testClipPlayer()
{
    bool ok = true;

    ssbb::ClipPlayer player;
    player.prepare(44100.0, 256);

    // Load 512 mono samples of value 0.5f.
    std::vector<float> samples(512, 0.5f);
    player.load(samples, 1);

    expect(!player.isPlaying(),   "ClipPlayer: not playing after load", ok);
    expect(player.totalFrames() == 512, "ClipPlayer: totalFrames", ok);

    player.play();
    expect(player.isPlaying(),    "ClipPlayer: playing after play()", ok);

    // Process a 256-sample block.
    std::vector<float> out0(512, 0.0f);
    std::vector<float> out1(512, 0.0f);
    float* outPtrs[2] = { out0.data(), out1.data() };

    player.processBlock(outPtrs, 2, 256);

    // All 256 output samples should be 0.5f (one channel → both outputs).
    bool mixOk = true;
    for (int i = 0; i < 256; ++i)
    {
        if (!nearEqF(out0[static_cast<std::size_t>(i)], 0.5f) ||
            !nearEqF(out1[static_cast<std::size_t>(i)], 0.5f))
        {
            mixOk = false;
            break;
        }
    }
    expect(mixOk, "ClipPlayer: first block mixed correctly", ok);
    expect(player.positionFrames() == 256, "ClipPlayer: position after first block", ok);

    // Process another 256-sample block — should exhaust the clip.
    std::fill(out0.begin(), out0.end(), 0.0f);
    player.processBlock(outPtrs, 2, 256);

    expect(!player.isPlaying(), "ClipPlayer: auto-stopped after clip end", ok);
    expect(player.positionFrames() >= 512, "ClipPlayer: position at or past end", ok);

    // Seek to start and verify position resets.
    player.seekToStart();
    expect(player.positionFrames() == 0, "ClipPlayer: seekToStart", ok);

    return ok;
}

// ---- Test: ClipPlayer — stop before clip exhausted -------------------------

bool testClipPlayerStop()
{
    bool ok = true;

    ssbb::ClipPlayer player;
    std::vector<float> samples(1024, 0.25f);
    player.load(samples, 1);
    player.play();

    float* outPtrs[1] = { nullptr };   // null output — still advances position
    // Swap in a real buffer so the call doesn't crash.
    std::vector<float> out(256, 0.0f);
    outPtrs[0] = out.data();

    player.processBlock(outPtrs, 1, 128);
    expect(player.isPlaying(),         "ClipPlayerStop: still playing", ok);
    expect(player.positionFrames() == 128, "ClipPlayerStop: pos after 128 frames", ok);

    player.stop();
    expect(!player.isPlaying(),        "ClipPlayerStop: stopped", ok);

    return ok;
}

// ---- Test: CommandHistory execute / undo / redo ----------------------------

struct DoubleVal
{
    double value { 0.0 };
};

struct SetValueCommand : ssbb::Command
{
    DoubleVal& target;
    double     oldVal;
    double     newVal;

    SetValueCommand(DoubleVal& t, double oldV, double newV)
        : target(t), oldVal(oldV), newVal(newV) {}

    void execute() noexcept override { target.value = newVal; }
    void undo()    noexcept override { target.value = oldVal; }
};

bool testCommandHistory()
{
    bool ok = true;

    ssbb::CommandHistory history;
    DoubleVal val;

    expect(!history.canUndo(), "CommandHistory: empty canUndo", ok);
    expect(!history.canRedo(), "CommandHistory: empty canRedo", ok);

    history.execute(std::make_unique<SetValueCommand>(val, 0.0, 1.0));
    expect(val.value == 1.0,   "CommandHistory: execute 1", ok);
    expect(history.canUndo(),  "CommandHistory: canUndo after execute", ok);
    expect(!history.canRedo(), "CommandHistory: canRedo after execute", ok);
    expect(history.undoDepth() == 1, "CommandHistory: undoDepth == 1", ok);

    history.execute(std::make_unique<SetValueCommand>(val, 1.0, 2.0));
    expect(val.value == 2.0, "CommandHistory: execute 2", ok);
    expect(history.undoDepth() == 2, "CommandHistory: undoDepth == 2", ok);

    history.undo();
    expect(val.value == 1.0, "CommandHistory: undo 1", ok);
    expect(history.canRedo(), "CommandHistory: canRedo after undo", ok);
    expect(history.redoDepth() == 1, "CommandHistory: redoDepth == 1", ok);

    history.undo();
    expect(val.value == 0.0, "CommandHistory: undo 2", ok);
    expect(!history.canUndo(), "CommandHistory: canUndo empty after 2 undos", ok);

    history.redo();
    expect(val.value == 1.0, "CommandHistory: redo 1", ok);

    history.redo();
    expect(val.value == 2.0, "CommandHistory: redo 2", ok);
    expect(!history.canRedo(), "CommandHistory: canRedo empty", ok);

    // A new execute after redo clears redo stack.
    history.undo();
    history.execute(std::make_unique<SetValueCommand>(val, 1.0, 99.0));
    expect(val.value == 99.0,  "CommandHistory: new execute after undo", ok);
    expect(!history.canRedo(), "CommandHistory: redo cleared after new execute", ok);

    return ok;
}

// ---- Test: CommandHistory depth limit ---------------------------------------

bool testCommandHistoryDepthLimit()
{
    bool ok = true;

    constexpr std::size_t kDepth = 4;
    ssbb::CommandHistory history(kDepth);
    DoubleVal val;

    for (int i = 1; i <= 6; ++i)
        history.execute(std::make_unique<SetValueCommand>(
            val, static_cast<double>(i - 1), static_cast<double>(i)));

    expect(history.undoDepth() == kDepth,
           "CommandHistoryDepth: undoDepth capped at maxDepth", ok);
    expect(val.value == 6.0, "CommandHistoryDepth: value after 6 executes", ok);

    // Undo all 4 kept steps — should reach value 2 (steps 1+2 were dropped).
    for (std::size_t i = 0; i < kDepth; ++i) history.undo();
    expect(val.value == 2.0, "CommandHistoryDepth: undo to oldest kept step", ok);

    return ok;
}

// ---- Test: CommandHistory no-op on empty stacks ----------------------------

bool testCommandHistoryNoOp()
{
    bool ok = true;
    ssbb::CommandHistory history;

    history.undo(); // should not crash
    history.redo(); // should not crash
    expect(!history.canUndo(), "CommandHistoryNoOp: canUndo empty", ok);
    expect(!history.canRedo(), "CommandHistoryNoOp: canRedo empty", ok);

    return ok;
}

// ---- Test: SessionDocument::migrate ----------------------------------------

bool testSessionMigrate()
{
    bool ok = true;

    // Known version — migration is a no-op.
    ssbb::SessionData known;
    known.version = ssbb::SessionDocument::kSchemaVersion;
    expect(ssbb::SessionDocument::migrate(known),
           "SessionMigrate: known version returns true", ok);
    expect(known.version == ssbb::SessionDocument::kSchemaVersion,
           "SessionMigrate: version unchanged", ok);

    // Unknown version — should return false.
    ssbb::SessionData unknown;
    unknown.version = 9999;
    expect(!ssbb::SessionDocument::migrate(unknown),
           "SessionMigrate: unknown version returns false", ok);

    return ok;
}

} // anonymous namespace

int main()
{
    bool allPassed = true;

    auto run = [&](bool result, std::string_view name)
    {
        if (!result)
        {
            std::cerr << "SUITE FAILED: " << name << '\n';
            allPassed = false;
        }
    };

    run(testWavRoundTripMono(),         "WavRoundTripMono");
    run(testWavRoundTripStereo(),       "WavRoundTripStereo");
    run(testWavReaderPcm16(),           "WavReaderPcm16");
    run(testWavReaderRejectsBadFile(),  "WavReaderRejectsBadFile");
    run(testClipPlayer(),               "ClipPlayer");
    run(testClipPlayerStop(),           "ClipPlayerStop");
    run(testCommandHistory(),           "CommandHistory");
    run(testCommandHistoryDepthLimit(), "CommandHistoryDepthLimit");
    run(testCommandHistoryNoOp(),       "CommandHistoryNoOp");
    run(testSessionMigrate(),           "SessionMigrate");

    if (allPassed)
    {
        std::cout << "All playback tests passed.\n";
        return 0;
    }
    return 1;
}
