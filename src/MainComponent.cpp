// MainComponent.cpp
// All code here runs on the JUCE message thread.
// No audio-thread rules apply; JUCE APIs and String construction are fine.
#include "MainComponent.h"

namespace ssbb {

MainComponent::MainComponent(AudioEngine& engine)
    : engine_(engine),
      // AudioDeviceSelectorComponent has no default constructor — must be here.
      deviceSelector_(engine.getDeviceManager(),
                      /*minInputChannels*/  0,
                      /*maxInputChannels*/  2,
                      /*minOutputChannels*/ 0,
                      /*maxOutputChannels*/ 2,
                      /*showMidiInputOptions*/    false,
                      /*showMidiOutputSelector*/  false,
                      /*showChannelsAsStereoPairs*/ true,
                      /*hideAdvancedOptionsWithButton*/ false)
{
    // ---- Device selector ----
    addAndMakeVisible(deviceSelector_);

    // ---- Play / Stop ----
    addAndMakeVisible(playStopButton_);
    playStopButton_.onClick = [this]
    {
        auto& transport = engine_.getTransport();
        if (transport.isPlaying())
        {
            transport.stop();
            playStopButton_.setButtonText("Play");
        }
        else
        {
            transport.play();
            playStopButton_.setButtonText("Stop");
        }
    };

    // ---- Tempo ----
    tempoLabel_.setText("BPM", juce::dontSendNotification);
    tempoLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(tempoLabel_);

    tempoSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    tempoSlider_.setRange(20.0, 300.0, 0.1);
    tempoSlider_.setValue(120.0, juce::dontSendNotification);
    tempoSlider_.setTooltip("Tempo (BPM)");
    tempoSlider_.onValueChange = [this]
    {
        engine_.getTransport().setTempo(tempoSlider_.getValue());
    };
    addAndMakeVisible(tempoSlider_);

    // ---- Time signature ----
    timeSigLabel_.setText("Sig:", juce::dontSendNotification);
    timeSigLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(timeSigLabel_);

    // Numerator: 1–16; item ID == value for direct use in setTimeSignature.
    for (int i = 1; i <= 16; ++i)
        numeratorBox_.addItem(juce::String(i), i);
    numeratorBox_.setSelectedId(4, juce::dontSendNotification);
    numeratorBox_.onChange = [this]
    {
        engine_.getTransport().setTimeSignature(
            numeratorBox_.getSelectedId(),
            denominatorBox_.getSelectedId());
    };
    addAndMakeVisible(numeratorBox_);

    // "/" divider label between numerator and denominator boxes
    addAndMakeVisible(dividerLabel_);

    // Denominator: 2, 4, 8, 16; item ID == value.
    for (int d : { 2, 4, 8, 16 })
        denominatorBox_.addItem(juce::String(d), d);
    denominatorBox_.setSelectedId(4, juce::dontSendNotification);
    denominatorBox_.onChange = [this]
    {
        engine_.getTransport().setTimeSignature(
            numeratorBox_.getSelectedId(),
            denominatorBox_.getSelectedId());
    };
    addAndMakeVisible(denominatorBox_);

    // ---- Loop ----
    loopToggle_.setToggleState(false, juce::dontSendNotification);
    loopToggle_.onClick = [this]
    {
        engine_.getTransport().setLoopEnabled(loopToggle_.getToggleState());
    };
    addAndMakeVisible(loopToggle_);

    // ---- Metronome ----
    metronomeToggle_.setToggleState(false, juce::dontSendNotification);
    metronomeToggle_.onClick = [this]
    {
        engine_.getMetronome().setEnabled(metronomeToggle_.getToggleState());
    };
    addAndMakeVisible(metronomeToggle_);

    // ---- Info label ----
    infoLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(infoLabel_);

    // Prime transport defaults so the engine state matches the UI.
    engine_.getTransport().setTempo(120.0);
    engine_.getTransport().setTimeSignature(4, 4);

    startTimerHz(20);          // 50 ms refresh for info label / button sync
    setSize(700, 500);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    // Top: device selector gets most of the vertical space.
    deviceSelector_.setBounds(bounds.removeFromTop(340));
    bounds.removeFromTop(8);

    // Transport row
    auto row = bounds.removeFromTop(36);
    playStopButton_.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(8);
    tempoLabel_.setBounds(row.removeFromLeft(40));
    tempoSlider_.setBounds(row.removeFromLeft(120));
    row.removeFromLeft(8);
    timeSigLabel_.setBounds(row.removeFromLeft(30));
    numeratorBox_.setBounds(row.removeFromLeft(48));
    dividerLabel_.setBounds(row.removeFromLeft(16));
    denominatorBox_.setBounds(row.removeFromLeft(48));
    row.removeFromLeft(8);
    loopToggle_.setBounds(row.removeFromLeft(60));
    row.removeFromLeft(8);
    metronomeToggle_.setBounds(row.removeFromLeft(90));

    // Info label
    bounds.removeFromTop(8);
    infoLabel_.setBounds(bounds.removeFromTop(28));
}

// ---- Timer (20 Hz) ----

void MainComponent::timerCallback()
{
    // Keep the play/stop button label in sync with the true transport state
    // (handles external stops such as device errors).
    playStopButton_.setButtonText(
        engine_.getTransport().isPlaying() ? "Stop" : "Play");

    updateInfoLabel();
}

void MainComponent::updateInfoLabel()
{
    const int    inCh    = engine_.getNumInputChannels();
    const int    outCh   = engine_.getNumOutputChannels();
    const double latMs   = engine_.getEstimatedLatencyMs();
    const double bpm     = engine_.getTransport().getTempo();
    const double sr      = engine_.getTransport().getSampleRate();
    const int64_t posSamp = engine_.getTransport().getPositionInSamples();

    const double posBeats = (bpm > 0.0 && sr > 0.0)
                                ? Transport::samplesToBeats(posSamp, bpm, sr)
                                : 0.0;

    juce::String info;
    info << "In: "         << inCh  << " ch"
         << "  |  Out: "   << outCh << " ch"
         << "  |  Latency: " << juce::String(latMs,    1) << " ms"
         << "  |  Position: " << juce::String(posBeats, 3) << " beats"
         << "  |  " << juce::String(bpm, 1) << " BPM";

    infoLabel_.setText(info, juce::dontSendNotification);
}

} // namespace ssbb
