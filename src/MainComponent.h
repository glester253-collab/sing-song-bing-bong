#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "VocalTrack.h"
#include <memory>
#include <vector>

namespace ssbb {

/// MainComponent
/// Hosts the JUCE AudioDeviceSelectorComponent plus a transport control row,
/// a vocal-track recording row, and a one-line info label updated at 20 Hz
/// via a JUCE Timer.
///
/// All UI interactions call Transport / Metronome / VocalTrack setters on the
/// message thread — never from inside the audio callback.
class MainComponent final : public juce::Component,
                             private juce::Timer
{
public:
    explicit MainComponent(AudioEngine& engine);
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateInfoLabel();
    void startImport();
    void startExport();

    AudioEngine& engine_;

    // Device selector (must be initialised in the member-initialiser list
    // because AudioDeviceSelectorComponent has no default constructor).
    juce::AudioDeviceSelectorComponent deviceSelector_;

    // Transport controls
    juce::TextButton   playStopButton_  { "Play" };
    juce::TextButton   rewindButton_    { "Rewind" };
    juce::Label        tempoLabel_;
    juce::Slider       tempoSlider_;
    juce::Label        timeSigLabel_;
    juce::ComboBox     numeratorBox_;
    juce::Label        dividerLabel_    { {}, "/" };
    juce::ComboBox     denominatorBox_;
    juce::ToggleButton loopToggle_      { "Loop" };
    juce::ToggleButton metronomeToggle_ { "Metronome" };

    // Vocal-track recording controls
    juce::ToggleButton armButton_     { "Arm" };
    juce::ToggleButton monitorButton_ { "Monitor" };
    juce::TextButton   recordButton_  { "Record" };
    juce::TextButton   importButton_  { "Import WAV" };
    juce::TextButton   exportButton_  { "Export WAV" };
    juce::TextButton   recoverButton_ { "Recover" };
    juce::Label        trimStartLabel_ { {}, "Trim start" };
    juce::Slider       trimStartSlider_;
    juce::Label        trimEndLabel_   { {}, "Trim end" };
    juce::Slider       trimEndSlider_;
    juce::Label        moveLabel_      { {}, "Move" };
    juce::Slider       moveSlider_;

    std::unique_ptr<juce::FileChooser> fileChooser_;

    juce::Rectangle<int> waveformBounds_;
    std::vector<WaveformCache::Frame> waveformFrames_;
    uint64_t waveformGeneration_ { 0 };
    int timerTicks_ { 0 };

    // Status bar
    juce::Label infoLabel_;
};

} // namespace ssbb
