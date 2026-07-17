#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "VocalTrack.h"

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

    void resized() override;

private:
    void timerCallback() override;
    void updateInfoLabel();

    AudioEngine& engine_;

    // Device selector (must be initialised in the member-initialiser list
    // because AudioDeviceSelectorComponent has no default constructor).
    juce::AudioDeviceSelectorComponent deviceSelector_;

    // Transport controls
    juce::TextButton   playStopButton_  { "Play" };
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

    // Status bar
    juce::Label infoLabel_;
};

} // namespace ssbb
