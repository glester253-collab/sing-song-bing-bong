#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "VocalTrack.h"
#include "CommandHistory.h"
#include "SessionDocument.h"

namespace ssbb {

/// MainComponent
/// Hosts the JUCE AudioDeviceSelectorComponent plus transport controls,
/// vocal-track recording, loop-range editors, metronome level, WAV import/export,
/// autosave, and crash-recovery UI.
///
/// All UI interactions run on the message thread — never inside the audio callback.
class MainComponent final : public juce::Component,
                             private juce::Timer
{
public:
    explicit MainComponent(AudioEngine& engine);
    ~MainComponent() override;

    void resized() override;

private:
    // ---- Timer callbacks -------------------------------------------------
    void timerCallback() override;
    void updateInfoLabel();

    // ---- Persistence helpers (message thread) ----------------------------
    void saveDeviceSettings();
    void loadDeviceSettings();

    // ---- Autosave / crash recovery (message thread) ---------------------
    void autosave();
    void checkForRecovery();

    // ---- WAV import / export (message thread) ----------------------------
    void importWav();
    void exportWav();

    // ---- Data -----------------------------------------------------------
    AudioEngine&    engine_;
    CommandHistory  commandHistory_;
    SessionData     sessionData_;
    juce::File      sessionFile_;
    juce::File      deviceSettingsFile_;

    int  autosaveTickCount_ { 0 };   // incremented each timer tick (20 Hz)

    // ---- UI: device selector --------------------------------------------
    juce::AudioDeviceSelectorComponent deviceSelector_;

    // ---- UI: transport row ----------------------------------------------
    juce::TextButton   playStopButton_   { "Play" };
    juce::TextButton   rewindButton_     { "|<" };
    juce::Label        tempoLabel_;
    juce::Slider       tempoSlider_;
    juce::Label        timeSigLabel_;
    juce::ComboBox     numeratorBox_;
    juce::Label        dividerLabel_     { {}, "/" };
    juce::ComboBox     denominatorBox_;
    juce::ToggleButton loopToggle_       { "Loop" };

    // ---- UI: loop range row ---------------------------------------------
    juce::Label     loopRangeLabel_;
    juce::Label     loopStartLabel_;
    juce::TextEditor loopStartEditor_;
    juce::Label     loopEndLabel_;
    juce::TextEditor loopEndEditor_;
    juce::TextButton loopSetButton_     { "Set Loop" };

    // ---- UI: metronome row ----------------------------------------------
    juce::ToggleButton metronomeToggle_ { "Metronome" };
    juce::Label        metroLevelLabel_;
    juce::Slider       metroLevelSlider_;

    // ---- UI: recording row ----------------------------------------------
    juce::ToggleButton armButton_      { "Arm" };
    juce::ToggleButton monitorButton_  { "Monitor" };
    juce::TextButton   recordButton_   { "Record" };

    // ---- UI: WAV import/export row --------------------------------------
    juce::TextButton importWavButton_  { "Import WAV" };
    juce::TextButton exportWavButton_  { "Export WAV" };

    // ---- UI: undo/redo row ----------------------------------------------
    juce::TextButton undoButton_       { "Undo" };
    juce::TextButton redoButton_       { "Redo" };

    // ---- UI: status bar -------------------------------------------------
    juce::Label infoLabel_;
};

} // namespace ssbb

