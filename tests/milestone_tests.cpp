// milestone_tests.cpp
// Plain C++20 tests for Milestone 2-5 components.
// Follows the same expect() / main() pattern as transport_tests.cpp.
//
// Tested:
//   M2: DrumPad, PadEngine, StepSequencer, MidiRecorder,
//       SubtractiveSynth, BounceRenderer
//   M3: MixerChannel, VocalChain, AutomationLane, LoudnessMeter
//   M4: FeatureExtractor, MixProposal, MixAssistant
//   M5: ControllerMap, PresetBrowser, PluginHostInterface, DiagnosticsBundle

#include "DrumPad.h"
#include "PadEngine.h"
#include "StepSequencer.h"
#include "MidiRecorder.h"
#include "SubtractiveSynth.h"
#include "BounceRenderer.h"
#include "MixerChannel.h"
#include "VocalChain.h"
#include "AutomationLane.h"
#include "LoudnessMeter.h"
#include "FeatureExtractor.h"
#include "MixProposal.h"
#include "MixAssistant.h"
#include "ControllerMap.h"
#include "PresetBrowser.h"
#include "PluginHostInterface.h"
#include "DiagnosticsBundle.h"
#include "SampleBrowser.h"
#include "WavWriter.h"
#include "WavReader.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, std::string_view msg, bool& success)
{
    if (!condition)
    {
        std::cerr << "[FAIL] " << msg << '\n';
        success = false;
    }
}

// ---- TempDir helper -------------------------------------------------------
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
    }
    std::filesystem::path operator/(std::string_view n) const { return path / n; }
};

// ---- Write a tiny float WAV for use in WavReader/FeatureExtractor tests ---
bool writeTinyWav(const std::filesystem::path& p,
                  double sr = 44100.0, int nCh = 1, int nFrames = 1024)
{
    ssbb::WavWriter w;
    if (!w.open(p, sr, nCh)) return false;
    std::vector<float> buf(static_cast<std::size_t>(nFrames * nCh), 0.1f);
    w.write(buf.data(), static_cast<int>(buf.size()));
    w.close();
    return true;
}

} // namespace

// ===========================================================================
int main()
{
    bool ok = true;

    // -----------------------------------------------------------------------
    // M2.1 — DrumPad
    // -----------------------------------------------------------------------
    {
        ssbb::DrumPad pad;
        pad.notePitch = 36;
        pad.name      = "Kick";
        pad.defaultVelocity = 100;
        expect(pad.notePitch == 36,          "DrumPad: notePitch", ok);
        expect(pad.name == "Kick",           "DrumPad: name",      ok);
        expect(pad.defaultVelocity == 100,   "DrumPad: velocity",  ok);
        expect(ssbb::DrumPad::kNumPads == 16, "DrumPad::kNumPads == 16", ok);
    }

    // -----------------------------------------------------------------------
    // M2.2 — PadEngine
    // -----------------------------------------------------------------------
    {
        ssbb::PadEngine engine;
        engine.prepare(44100.0, 512);

        ssbb::DrumPad pad;
        pad.notePitch = 36;
        pad.name = "Kick";
        engine.setPad(0, pad);

        const auto& retrieved = engine.getPad(0);
        expect(retrieved.name == "Kick",     "PadEngine: setPad/getPad", ok);

        // triggerPad should succeed when queue is not full.
        bool fired = engine.triggerPad(0, 100, 0);
        expect(fired,                        "PadEngine: triggerPad returns true", ok);

        // processBlock with silence (no sample loaded — voices should stay silent).
        std::array<float, 512> outL {};
        float* ptrs[2] = { outL.data(), outL.data() };
        engine.processBlock(ptrs, 2, 512);
        // No crash; output stays silent (no WAV loaded).
        expect(outL[0] == 0.0f,              "PadEngine: no sample = silence", ok);
    }

    // -----------------------------------------------------------------------
    // M2.3 — StepSequencer
    // -----------------------------------------------------------------------
    {
        ssbb::StepSequencer seq;
        seq.prepare(44100.0, 512);
        seq.setTempo(120.0);
        seq.setNumSteps(16);
        seq.setEnabled(true);

        // Set a step on pad 0, step 0.
        ssbb::StepSequencer::Step s;
        s.active   = true;
        s.velocity = 100;
        seq.setStep(0, 0, s);

        const auto got = seq.getStep(0, 0);
        expect(got.active,                   "StepSequencer: setStep/getStep", ok);
        expect(got.velocity == 100,          "StepSequencer: step velocity",   ok);

        // Process a block at sample 0 — step 0 should fire.
        int hitCount = 0;
        seq.setHitCallback([&](int pad, int vel, int64_t t)
        {
            (void)t;
            if (pad == 0 && vel == 100) ++hitCount;
        });

        // Step 0 fires at sample 0 when isPlaying = true.
        seq.process(0, 512, /*isPlaying=*/true);
        expect(hitCount > 0,                 "StepSequencer: step fires on process", ok);

        // Disabled sequencer should not fire.
        seq.setEnabled(false);
        hitCount = 0;
        seq.process(0, 512, /*isPlaying=*/true);
        expect(hitCount == 0,                "StepSequencer: disabled = no hits", ok);
    }

    // -----------------------------------------------------------------------
    // M2.4 — MidiRecorder
    // -----------------------------------------------------------------------
    {
        ssbb::MidiRecorder rec;
        rec.prepare(44100.0, 512);
        rec.startRecording();
        expect(rec.isRecording(),            "MidiRecorder: isRecording", ok);

        // Append a note-on event.
        rec.appendEvent(0x90, 60, 100, 0);

        // Drain to store.
        rec.drainToStore();
        expect(rec.getEvents().size() == 1,  "MidiRecorder: one event stored", ok);
        expect(rec.getEvents()[0].status == 0x90, "MidiRecorder: status byte", ok);
        expect(rec.getEvents()[0].data1  == 60,   "MidiRecorder: note number", ok);

        // Quantize (should not crash on 1 event).
        rec.quantize(120.0, 16, 1.0f);
        expect(rec.getEvents().size() == 1,  "MidiRecorder: quantize keeps events", ok);

        rec.stopRecording();
        expect(!rec.isRecording(),           "MidiRecorder: stopRecording", ok);

        rec.clearEvents();
        expect(rec.getEvents().empty(),      "MidiRecorder: clearEvents", ok);
    }

    // -----------------------------------------------------------------------
    // M2.5 — SubtractiveSynth
    // -----------------------------------------------------------------------
    {
        ssbb::SubtractiveSynth synth;
        synth.prepare(44100.0, 512);
        synth.setFilterCutoff(2000.0f);
        synth.setFilterResonance(0.3f);
        synth.setADSR(0.01f, 0.1f, 0.7f, 0.2f);
        synth.setGain(1.0f);

        synth.noteOn(60, 100);

        std::array<float, 512> outL {}, outR {};
        synth.processBlock(outL.data(), outR.data(), 512);

        // After note-on with attack, output should be non-zero.
        bool hasAudio = false;
        for (auto s : outL) if (s != 0.0f) { hasAudio = true; break; }
        expect(hasAudio,                     "SubtractiveSynth: note-on produces audio", ok);

        // NaN/Inf check.
        bool hasNan = false;
        for (auto s : outL) if (s != s || (s > 4.0f) || (s < -4.0f)) { hasNan = true; break; }
        expect(!hasNan,                      "SubtractiveSynth: no NaN/Inf/clip", ok);

        // NoteOff should silence the output after release.
        synth.noteOff(60);
        for (int block = 0; block < 50; ++block)
        {
            outL.fill(0.0f);
            synth.processBlock(outL.data(), outR.data(), 512);
        }
        float finalPeak = 0.0f;
        for (auto s : outL) { const float a = s < 0 ? -s : s; if (a > finalPeak) finalPeak = a; }
        expect(finalPeak < 0.01f,            "SubtractiveSynth: silence after release", ok);
    }

    // -----------------------------------------------------------------------
    // M2.6 — BounceRenderer
    // -----------------------------------------------------------------------
    {
        TempDir tmp("ssbb_bounce_test");
        const auto outPath = tmp / "bounce.wav";

        int callCount = 0;
        ssbb::BounceRenderer renderer;

        const bool ok2 = renderer.render(
            outPath,
            [&](float** out, int ch, int ns, int64_t pos)
            {
                (void)pos;
                for (int c = 0; c < ch; ++c)
                    for (int i = 0; i < ns; ++i)
                        out[c][i] = 0.5f;
                ++callCount;
            },
            44100.0,
            1,
            44100  // 1 second
        );

        expect(ok2,                          "BounceRenderer: render returns true", ok);
        expect(callCount > 0,                "BounceRenderer: callback was called", ok);
        expect(std::filesystem::exists(outPath), "BounceRenderer: output file exists", ok);

        // Verify we can read the output WAV back.
        ssbb::WavReader reader;
        const bool loaded = reader.load(outPath);
        expect(loaded,                       "BounceRenderer: output is valid WAV", ok);
        expect(reader.numFrames() == 44100,  "BounceRenderer: correct frame count", ok);
    }

    // -----------------------------------------------------------------------
    // M3.1 — MixerChannel
    // -----------------------------------------------------------------------
    {
        ssbb::MixerChannel ch;
        ch.setGain(0.5f);
        ch.setPan(0.0f);
        ch.setMute(false);

        constexpr int N = 256;
        std::array<float, N> in {};
        for (auto& s : in) s = 1.0f;

        std::array<float, N> busL {}, busR {};
        ch.processBlock(in.data(),
                        busL.data(), busR.data(),
                        nullptr, nullptr, nullptr, nullptr,
                        N, /*anySoloed=*/false);

        // With gain 0.5 and centre pan, both buses should have ~0.5.
        expect(busL[0] > 0.0f,               "MixerChannel: signal in main bus L", ok);
        expect(busR[0] > 0.0f,               "MixerChannel: signal in main bus R", ok);

        // Mute should silence.
        ch.setMute(true);
        busL.fill(0.0f); busR.fill(0.0f);
        ch.processBlock(in.data(),
                        busL.data(), busR.data(),
                        nullptr, nullptr, nullptr, nullptr,
                        N, false);
        expect(busL[0] == 0.0f,              "MixerChannel: mute silences output", ok);

        // Peak meters should update.
        ch.setMute(false);
        ch.setGain(1.0f);
        busL.fill(0.0f); busR.fill(0.0f);
        ch.processBlock(in.data(),
                        busL.data(), busR.data(),
                        nullptr, nullptr, nullptr, nullptr,
                        N, false);
        expect(ch.getPeakLevel(0) > 0.0f,   "MixerChannel: peak meter L", ok);
    }

    // -----------------------------------------------------------------------
    // M3.2 — VocalChain
    // -----------------------------------------------------------------------
    {
        ssbb::VocalChain chain;
        chain.prepare(44100.0, 512);

        std::array<float, 512> buf {};
        for (auto& s : buf) s = 0.5f;

        chain.processBlock(buf.data(), 512);

        // With HPF on and compressor on, output should be non-zero but bounded.
        bool nonZero = false;
        bool bounded = true;
        for (auto s : buf)
        {
            if (s != 0.0f) nonZero = true;
            if (s > 4.0f || s < -4.0f || s != s) bounded = false;
        }
        expect(nonZero,                      "VocalChain: produces output", ok);
        expect(bounded,                      "VocalChain: output is bounded", ok);

        // Silence input should produce silence (or near-silence after filters settle).
        buf.fill(0.0f);
        for (int i = 0; i < 20; ++i) chain.processBlock(buf.data(), 512);
        float maxOut = 0.0f;
        for (auto s : buf) { const float a = s < 0 ? -s : s; if (a > maxOut) maxOut = a; }
        expect(maxOut < 0.01f,               "VocalChain: silence-in → silence-out", ok);
    }

    // -----------------------------------------------------------------------
    // M3.3 — AutomationLane
    // -----------------------------------------------------------------------
    {
        ssbb::AutomationLane lane("Gain");
        expect(lane.name() == "Gain",        "AutomationLane: name", ok);

        lane.addPoint(0,    0.0f);
        lane.addPoint(1000, 1.0f);
        lane.addPoint(2000, 0.5f);

        // Before first point.
        expect(lane.evaluate(-100, 0.0f) == 0.0f,  "AutomationLane: before first pt", ok);
        // After last point.
        expect(lane.evaluate(3000, 0.0f) == 0.5f,  "AutomationLane: after last pt",   ok);
        // Midpoint interpolation.
        const float mid = lane.evaluate(500, 0.0f);
        expect(mid > 0.0f && mid < 1.0f,           "AutomationLane: interpolation",   ok);

        lane.clearPoints();
        expect(lane.evaluate(500, 42.0f) == 42.0f, "AutomationLane: cleared = default", ok);
    }

    // -----------------------------------------------------------------------
    // M3.4 — LoudnessMeter
    // -----------------------------------------------------------------------
    {
        ssbb::LoudnessMeter meter;
        meter.prepare(44100.0, 2);

        // Feed 1 second of 0.1 FS sine tone.
        constexpr int kRate = 44100;
        constexpr int kBlock = 512;
        std::array<float, kBlock> chL {}, chR {};
        const float* ptrs[2] = { chL.data(), chR.data() };

        for (int block = 0; block < kRate / kBlock; ++block)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float t = static_cast<float>(block * kBlock + i) / static_cast<float>(kRate);
                chL[i] = chR[i] = 0.1f * std::sin(2.0f * 3.14159265f * 440.0f * t);
            }
            meter.processBlock(ptrs, 2, kBlock);
        }

        const float peak = meter.getPeakDb();
        expect(peak > -25.0f && peak < 0.0f,  "LoudnessMeter: peak in range", ok);
        // Integrated LUFS should be negative.
        const float lufs = meter.getIntegratedLufs();
        expect(lufs < 0.0f,                   "LoudnessMeter: integrated LUFS < 0", ok);
    }

    // -----------------------------------------------------------------------
    // M4.1 — FeatureExtractor
    // -----------------------------------------------------------------------
    {
        TempDir tmp("ssbb_feat_test");
        const auto wavPath = tmp / "test.wav";
        expect(writeTinyWav(wavPath),          "FeatureExtractor: write test WAV", ok);

        ssbb::FeatureExtractor extractor;
        const auto feats = extractor.analyseFile(wavPath);

        expect(feats.valid,                    "FeatureExtractor: valid result", ok);
        expect(feats.numFrames == 1024,        "FeatureExtractor: frame count", ok);
        expect(feats.peakDb > -60.0f,          "FeatureExtractor: peak > -60 dB", ok);
        expect(feats.rmsDb < 0.0f,             "FeatureExtractor: RMS < 0 dB", ok);
    }

    // -----------------------------------------------------------------------
    // M4.2 — MixProposal
    // -----------------------------------------------------------------------
    {
        ssbb::MixProposal proposal;
        proposal.summary  = "Test proposal";
        proposal.approved = true;

        bool applied = false;
        bool undone  = false;
        proposal.applyFn = [&] { applied = true; };
        proposal.undoFn  = [&] { undone  = true; };

        expect(proposal.apply(),              "MixProposal: apply() returns true", ok);
        expect(applied,                       "MixProposal: applyFn called", ok);
        expect(proposal.isApplied(),          "MixProposal: isApplied true", ok);

        expect(proposal.undoApply(),          "MixProposal: undoApply returns true", ok);
        expect(undone,                        "MixProposal: undoFn called", ok);
        expect(!proposal.isApplied(),         "MixProposal: isApplied false after undo", ok);
    }

    // -----------------------------------------------------------------------
    // M4.3 — MixAssistant
    // -----------------------------------------------------------------------
    {
        ssbb::FeatureSet feats;
        feats.valid            = true;
        feats.integratedLufs   = -20.0f;   // too quiet for streaming target
        feats.truePeakLinear   = 0.5f;     // well below ceiling
        feats.peakDb           = -6.0f;
        feats.rmsDb            = -20.0f;
        feats.crestFactorDb    = 14.0f;
        feats.lowEnergyRatio   = 0.20f;
        feats.midEnergyRatio   = 0.65f;
        feats.highEnergyRatio  = 0.15f;

        const auto target = ssbb::masteringTargetFor(ssbb::DeliveryPlatform::Streaming);

        ssbb::MixAssistant assistant;
        auto proposal = assistant.analyse(feats, target,
                                          [](const std::string&) { return 0.0f; });

        expect(!proposal.summary.empty(),     "MixAssistant: summary not empty", ok);
        expect(!proposal.changes.empty(),     "MixAssistant: has suggestions", ok);
        expect(proposal.confidence > 0.0f,   "MixAssistant: confidence > 0", ok);

        // Gain-staging suggestion should push toward -14 LUFS.
        bool hasGainSugg = false;
        for (const auto& c : proposal.changes)
            if (c.parameterId == "output_gain_db") hasGainSugg = true;
        expect(hasGainSugg,                   "MixAssistant: gain-staging suggestion", ok);
    }

    // -----------------------------------------------------------------------
    // M4.4 — Mastering targets
    // -----------------------------------------------------------------------
    {
        const auto st = ssbb::masteringTargetFor(ssbb::DeliveryPlatform::Streaming);
        expect(st.targetLufs == -14.0f,       "MasteringTarget: streaming LUFS", ok);

        const auto cd = ssbb::masteringTargetFor(ssbb::DeliveryPlatform::CD);
        expect(cd.targetLufs == -9.0f,        "MasteringTarget: CD LUFS", ok);

        const auto pod = ssbb::masteringTargetFor(ssbb::DeliveryPlatform::Podcast);
        expect(pod.targetLufs == -16.0f,      "MasteringTarget: podcast LUFS", ok);
    }

    // -----------------------------------------------------------------------
    // M5.1 — ControllerMap
    // -----------------------------------------------------------------------
    {
        ssbb::ControllerMap map;

        ssbb::ControllerMapping m;
        m.cc       = 7;   // Volume CC
        m.channel  = 0;   // omni
        m.paramId  = "output_gain_db";
        m.minValue = -60.0f;
        m.maxValue = 0.0f;
        map.addMapping(m);

        expect(map.getMappings().size() == 1,  "ControllerMap: addMapping", ok);

        // Process a CC event.
        float receivedValue = 999.0f;
        map.setChangeCallback([&](const std::string& id, float val)
        {
            if (id == "output_gain_db") receivedValue = val;
        });

        map.processCCEvent(7, 64, 1);   // mid-range value
        expect(receivedValue > -60.0f && receivedValue < 0.0f,
               "ControllerMap: CC dispatched to callback", ok);

        // MIDI learn.
        map.startLearning("filter_cutoff_hz", 20.0f, 20000.0f);
        expect(map.isLearning(),               "ControllerMap: isLearning", ok);
        expect(map.learningParamId() == "filter_cutoff_hz", "ControllerMap: learningParamId", ok);

        map.processCCEvent(74, 100, 1);  // CC 74 = classic filter cutoff
        expect(!map.isLearning(),              "ControllerMap: learn completed", ok);
        expect(map.getMappings().size() == 2,  "ControllerMap: new mapping added", ok);

        // Persistence round-trip.
        TempDir tmp("ssbb_ctrl_test");
        const auto p = tmp / "ctrl.json";
        expect(map.save(p),                    "ControllerMap: save", ok);
        ssbb::ControllerMap map2;
        expect(map2.load(p),                   "ControllerMap: load", ok);
        expect(map2.getMappings().size() == 2, "ControllerMap: round-trip count", ok);

        // Remove mapping.
        map.removeMapping("output_gain_db");
        expect(map.getMappings().size() == 1,  "ControllerMap: removeMapping", ok);
    }

    // -----------------------------------------------------------------------
    // M5.2 — PresetBrowser
    // -----------------------------------------------------------------------
    {
        TempDir tmp("ssbb_preset_test");
        ssbb::PresetBrowser browser;
        browser.setUserPresetsDir(tmp.path);

        // Save a preset.
        const bool saved = browser.saveUserPreset(
            "MyVocal", "Vocal",
            [](ssbb::Preset& p)
            {
                p.values["hpf_hz"]    = 80.0f;
                p.values["comp_ratio"]= 4.0f;
            });
        expect(saved,                           "PresetBrowser: saveUserPreset", ok);

        browser.rescan();
        expect(browser.getPresets().size() >= 1, "PresetBrowser: rescan finds preset", ok);

        // Apply the preset.
        float hpfApplied = 0.0f;
        browser.applyPreset("MyVocal",
            [&](const ssbb::Preset& p)
            {
                auto it = p.values.find("hpf_hz");
                if (it != p.values.end()) hpfApplied = it->second;
            });
        expect(hpfApplied == 80.0f,             "PresetBrowser: applyPreset values", ok);

        // Delete the preset.
        browser.deleteUserPreset("MyVocal");
        browser.rescan();
        expect(browser.getPresets().empty(),    "PresetBrowser: delete removes preset", ok);
    }

    // -----------------------------------------------------------------------
    // M5.3 — PluginHostInterface (NullPluginHost stub)
    // -----------------------------------------------------------------------
    {
        ssbb::NullPluginHost host;
        expect(!host.supportsFormat(ssbb::PluginFormat::VST3),
               "NullPluginHost: no VST3", ok);
        expect(!host.supportsFormat(ssbb::PluginFormat::CLAP),
               "NullPluginHost: no CLAP", ok);

        ssbb::PluginInfo info;
        auto instance = host.loadPlugin(info);
        expect(instance == nullptr,            "NullPluginHost: loadPlugin = nullptr", ok);

        auto results = host.scanDirectory("/tmp");
        expect(results.empty(),                "NullPluginHost: scan = empty", ok);
    }

    // -----------------------------------------------------------------------
    // M5.4 — DiagnosticsBundle
    // -----------------------------------------------------------------------
    {
        ssbb::DiagnosticsBundle bundle;
        bundle.setDeviceInfo("CoreAudio: 44100 Hz / 512 samples");
        bundle.setSessionInfo("3 takes, 2 clips, 4.2 s");
        bundle.setAudioSpec(44100.0, 512, 1, 2, 11.6);
        bundle.addLogLine("Test log line 1");
        bundle.addLogLine("Test log line 2");

        const auto report = bundle.buildReport();
        expect(report.find("Sing Song Bing Bong") != std::string::npos,
               "DiagnosticsBundle: has header", ok);
        expect(report.find("44100") != std::string::npos,
               "DiagnosticsBundle: has sample rate", ok);
        expect(report.find("Test log line 1") != std::string::npos,
               "DiagnosticsBundle: has log lines", ok);

        TempDir tmp("ssbb_diag_test");
        const auto p = tmp / "diag.txt";
        expect(bundle.writeTo(p),              "DiagnosticsBundle: writeTo", ok);
        expect(std::filesystem::exists(p),     "DiagnosticsBundle: file exists", ok);
    }

    // -----------------------------------------------------------------------
    // M5.5 — SampleBrowser
    // -----------------------------------------------------------------------
    {
        TempDir tmp("ssbb_sample_test");

        // Write a sample WAV into the tmp directory.
        const auto wavPath = tmp / "kick.wav";
        writeTinyWav(wavPath);

        ssbb::SampleBrowser browser;
        browser.setUserSamplesDir(tmp.path);
        browser.rescan();

        expect(!browser.getEntries().empty(),   "SampleBrowser: finds WAV file", ok);
        expect(browser.getEntries()[0].name == "kick",
               "SampleBrowser: name without extension", ok);
        expect(browser.getEntries()[0].ownership == ssbb::SampleOwnership::UserOwned,
               "SampleBrowser: user-owned", ok);
        expect(browser.isSafeToUse(wavPath),    "SampleBrowser: user file safe to use", ok);
    }

    // -----------------------------------------------------------------------
    std::cout << (ok ? "All milestone tests passed.\n" : "SOME TESTS FAILED.\n");
    return ok ? 0 : 1;
}
