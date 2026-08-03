// MainComponent.cpp
// All code here runs on the JUCE message thread.
// No audio-thread rules apply; JUCE APIs and String construction are fine.
#include "MainComponent.h"

#include <filesystem>

namespace {

std::filesystem::path toPath(const juce::File& file)
{
#if defined(_WIN32)
    return std::filesystem::path(file.getFullPathName().toWideCharPointer());
#else
    return std::filesystem::path(file.getFullPathName().toStdString());
#endif
}

} // namespace

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

    addAndMakeVisible(rewindButton_);
    rewindButton_.onClick = [this]
    {
        engine_.getTransport().setPositionInBeats(0.0);
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

    // ---- Arm ----
    // Toggles the VocalTrack between Armed and Idle.
    armButton_.setToggleState(false, juce::dontSendNotification);
    armButton_.onClick = [this]
    {
        auto& vt = engine_.getVocalTrack();
        if (armButton_.getToggleState())
            vt.arm();
        else
            vt.disarm();
    };
    addAndMakeVisible(armButton_);

    // ---- Monitor ----
    // Enables live input monitoring (dry input → output) without recording.
    monitorButton_.setToggleState(false, juce::dontSendNotification);
    monitorButton_.onClick = [this]
    {
        auto& vt = engine_.getVocalTrack();
        if (monitorButton_.getToggleState())
            vt.startMonitoring();
        else
            vt.stopMonitoring();
    };
    addAndMakeVisible(monitorButton_);

    // ---- Record ----
    // Starts recording if Armed/Monitoring; stops if Recording/Stopping.
    addAndMakeVisible(recordButton_);
    recordButton_.onClick = [this]
    {
        auto& vt = engine_.getVocalTrack();
        const auto state = vt.getState();
        if (state == VocalTrack::State::Recording ||
            state == VocalTrack::State::Stopping)
        {
            vt.stopRecording();
        }
        else if (state == VocalTrack::State::Armed ||
                 state == VocalTrack::State::Monitoring)
        {
            vt.startRecording();
        }
    };

    addAndMakeVisible(importButton_);
    importButton_.onClick = [this] { startImport(); };

    addAndMakeVisible(exportButton_);
    exportButton_.onClick = [this] { startExport(); };

    addAndMakeVisible(recoverButton_);
    recoverButton_.onClick = [this]
    {
        engine_.getVocalTrack().requestRecoveryLoad();
    };
    recoverButton_.setEnabled(engine_.getVocalTrack().recoveryAvailable());

    auto prepareEditSlider = [this](juce::Slider& slider)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
        slider.setTextValueSuffix(" s");
        slider.setRange(0.0, 30.0, 0.01);
        addAndMakeVisible(slider);
    };
    for (auto* label : { &trimStartLabel_, &trimEndLabel_, &moveLabel_ })
    {
        label->setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(*label);
    }
    prepareEditSlider(trimStartSlider_);
    prepareEditSlider(trimEndSlider_);
    prepareEditSlider(moveSlider_);

    trimStartSlider_.onValueChange = [this]
    {
        const double rate = engine_.getVocalTrack().getPlaybackSampleRate();
        engine_.getVocalTrack().setTrimStartSamples(
            static_cast<int64_t>(trimStartSlider_.getValue() * rate));
    };
    trimEndSlider_.onValueChange = [this]
    {
        const double rate = engine_.getVocalTrack().getPlaybackSampleRate();
        engine_.getVocalTrack().setTrimEndSamples(
            static_cast<int64_t>(trimEndSlider_.getValue() * rate));
    };
    moveSlider_.onValueChange = [this]
    {
        const double rate = engine_.getVocalTrack().getPlaybackSampleRate();
        engine_.getVocalTrack().setClipOffsetSamples(
            static_cast<int64_t>(moveSlider_.getValue() * rate));
    };

    // ---- Info label ----
    infoLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(infoLabel_);

    // Prime transport defaults so the engine state matches the UI.
    engine_.getTransport().setTempo(120.0);
    engine_.getTransport().setTimeSignature(4, 4);

    startTimerHz(20);          // 50 ms refresh for info label / button sync
    setSize(760, 720);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff17191d));

    auto area = waveformBounds_.toFloat();
    g.setColour(juce::Colour(0xff22262c));
    g.fillRoundedRectangle(area, 5.0f);
    g.setColour(juce::Colour(0xff59636f));
    g.drawRoundedRectangle(area, 5.0f, 1.0f);

    if (waveformFrames_.empty())
    {
        g.setColour(juce::Colour(0xffaeb7c2));
        g.drawFittedText("Record or import a WAV to see its waveform",
                         waveformBounds_.reduced(12),
                         juce::Justification::centred, 1);
        return;
    }

    const float centre = area.getCentreY();
    const float halfHeight = area.getHeight() * 0.43f;
    const int width = waveformBounds_.getWidth();
    g.setColour(juce::Colour(0xff62d6c5));
    for (int x = 0; x < width; ++x)
    {
        const auto index = static_cast<std::size_t>(
            static_cast<double>(x) * waveformFrames_.size() /
            static_cast<double>(width));
        const auto& frame = waveformFrames_[
            index < waveformFrames_.size() ? index : waveformFrames_.size() - 1];
        const float yTop = centre - juce::jlimit(0.0f, 1.0f, frame.peakPos) * halfHeight;
        const float yBottom = centre - juce::jlimit(-1.0f, 0.0f, frame.peakNeg) * halfHeight;
        const float screenX = area.getX() + static_cast<float>(x);
        g.drawVerticalLine(static_cast<int>(screenX), yTop, yBottom);
    }

    const auto totalFrames = engine_.getVocalTrack().getPlaybackLengthSamples();
    if (totalFrames > 0)
    {
        const auto trimStart = engine_.getVocalTrack().getTrimStartSamples();
        const auto trimEnd = engine_.getVocalTrack().getTrimEndSamples();
        const auto clipOffset = engine_.getVocalTrack().getClipOffsetSamples();
        const float trimLeft = static_cast<float>(trimStart) /
                               static_cast<float>(totalFrames);
        const float trimRight = static_cast<float>(trimEnd) /
                                static_cast<float>(totalFrames);
        g.setColour(juce::Colour(0x99000000));
        g.fillRect(area.withWidth(area.getWidth() * juce::jlimit(0.0f, 1.0f, trimLeft)));
        const float rightWidth = area.getWidth() * juce::jlimit(0.0f, 1.0f, trimRight);
        g.fillRect(juce::Rectangle<float>(area.getRight() - rightWidth,
                                          area.getY(), rightWidth, area.getHeight()));

        const auto position = engine_.getTransport().getPositionInSamples();
        const auto sourcePosition = position - clipOffset + trimStart;
        if (sourcePosition >= trimStart && sourcePosition < totalFrames - trimEnd)
        {
            const float fraction = juce::jlimit(
                0.0f, 1.0f,
                static_cast<float>(sourcePosition) / static_cast<float>(totalFrames));
            const float playhead = area.getX() + fraction * area.getWidth();
            g.setColour(juce::Colour(0xffffc857));
            g.drawVerticalLine(static_cast<int>(playhead), area.getY(), area.getBottom());
        }
    }
}

void MainComponent::startImport()
{
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Import an owned or licensed WAV", juce::File{}, "*.wav");
    const int flags = juce::FileBrowserComponent::openMode
                      | juce::FileBrowserComponent::canSelectFiles;
    fileChooser_->launchAsync(flags, [this](const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();
        if (!file.existsAsFile()) return;
        const auto path = toPath(file);

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Confirm audio rights")
                .withMessage("Import only audio you own or are licensed to use. "
                             "Do you have permission to use this WAV?")
                .withButton("Import")
                .withButton("Cancel"),
            [this, path](int result)
            {
                if (result == 1)
                    engine_.getVocalTrack().requestImport(path);
            });
    });
}

void MainComponent::startExport()
{
    if (!engine_.getVocalTrack().hasPlayback()) return;
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Export the latest take", juce::File{}, "*.wav");
    const int flags = juce::FileBrowserComponent::saveMode
                      | juce::FileBrowserComponent::canSelectFiles
                      | juce::FileBrowserComponent::warnAboutOverwriting;
    fileChooser_->launchAsync(flags, [this](const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();
        if (file == juce::File{}) return;
        if (!file.hasFileExtension("wav"))
            file = file.withFileExtension("wav");
        engine_.getVocalTrack().requestExport(toPath(file));
    });
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    // Top: device selector gets most of the vertical space.
    deviceSelector_.setBounds(bounds.removeFromTop(300));
    bounds.removeFromTop(8);

    // Transport row
    auto row = bounds.removeFromTop(36);
    playStopButton_.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(8);
    rewindButton_.setBounds(row.removeFromLeft(72));
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

    // Vocal-track recording row (below the transport row)
    bounds.removeFromTop(6);
    auto recRow = bounds.removeFromTop(36);
    armButton_.setBounds(recRow.removeFromLeft(60));
    recRow.removeFromLeft(8);
    monitorButton_.setBounds(recRow.removeFromLeft(80));
    recRow.removeFromLeft(8);
    recordButton_.setBounds(recRow.removeFromLeft(90));

    bounds.removeFromTop(6);
    auto fileRow = bounds.removeFromTop(36);
    importButton_.setBounds(fileRow.removeFromLeft(110));
    fileRow.removeFromLeft(8);
    exportButton_.setBounds(fileRow.removeFromLeft(110));
    fileRow.removeFromLeft(8);
    recoverButton_.setBounds(fileRow.removeFromLeft(90));

    bounds.removeFromTop(8);
    auto editRow = bounds.removeFromTop(36);
    trimStartLabel_.setBounds(editRow.removeFromLeft(72));
    trimStartSlider_.setBounds(editRow.removeFromLeft(150));
    editRow.removeFromLeft(8);
    trimEndLabel_.setBounds(editRow.removeFromLeft(64));
    trimEndSlider_.setBounds(editRow.removeFromLeft(150));
    editRow.removeFromLeft(8);
    moveLabel_.setBounds(editRow.removeFromLeft(48));
    moveSlider_.setBounds(editRow.removeFromLeft(150));

    bounds.removeFromTop(8);
    waveformBounds_ = bounds.removeFromTop(190);

    // Info label
    bounds.removeFromTop(8);
    infoLabel_.setBounds(bounds.removeFromTop(28));
}

// ---- Timer (20 Hz) ----

void MainComponent::timerCallback()
{
    ++timerTicks_;
    if (timerTicks_ >= 200)
    {
        timerTicks_ = 0;
        engine_.getVocalTrack().requestAutosave();
    }

    // Keep the play/stop button label in sync with the true transport state
    // (handles external stops such as device errors).
    playStopButton_.setButtonText(
        engine_.getTransport().isPlaying() ? "Stop" : "Play");

    // Sync vocal-track buttons with actual state (state can change on the
    // worker thread when the Stopping → Idle transition fires).
    const auto vtState = engine_.getVocalTrack().getState();

    armButton_.setToggleState(
        vtState != VocalTrack::State::Idle,
        juce::dontSendNotification);

    monitorButton_.setToggleState(
        vtState == VocalTrack::State::Monitoring,
        juce::dontSendNotification);

    if (vtState == VocalTrack::State::Recording ||
        vtState == VocalTrack::State::Stopping)
        recordButton_.setButtonText("Stop Rec");
    else
        recordButton_.setButtonText("Record");

    exportButton_.setEnabled(engine_.getVocalTrack().hasPlayback());
    recoverButton_.setEnabled(engine_.getVocalTrack().recoveryAvailable());

    if (engine_.getVocalTrack().copyWaveformIfChanged(
            waveformFrames_, waveformGeneration_))
    {
        const double rate = engine_.getVocalTrack().getPlaybackSampleRate();
        const double duration = rate > 0.0
            ? static_cast<double>(engine_.getVocalTrack().getPlaybackLengthSamples()) / rate
            : 0.0;
        trimStartSlider_.setRange(0.0, duration, 0.01);
        trimEndSlider_.setRange(0.0, duration, 0.01);
        if (rate > 0.0)
        {
            trimStartSlider_.setValue(
                static_cast<double>(engine_.getVocalTrack().getTrimStartSamples()) / rate,
                juce::dontSendNotification);
            trimEndSlider_.setValue(
                static_cast<double>(engine_.getVocalTrack().getTrimEndSamples()) / rate,
                juce::dontSendNotification);
            moveSlider_.setValue(
                static_cast<double>(engine_.getVocalTrack().getClipOffsetSamples()) / rate,
                juce::dontSendNotification);
        }
        repaint(waveformBounds_);
    }
    else if (engine_.getTransport().isPlaying())
        repaint(waveformBounds_);

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

    // Vocal-track state label
    const auto vtState = engine_.getVocalTrack().getState();
    juce::String vtLabel;
    switch (vtState)
    {
        case VocalTrack::State::Idle:       vtLabel = "Idle";       break;
        case VocalTrack::State::Armed:      vtLabel = "Armed";      break;
        case VocalTrack::State::Monitoring: vtLabel = "Monitoring"; break;
        case VocalTrack::State::Recording:  vtLabel = "REC";        break;
        case VocalTrack::State::Stopping:   vtLabel = "Stopping";   break;
        default:                            vtLabel = "?";           break;
    }

    juce::String info;
    info << "In: "           << inCh  << " ch"
         << "  |  Out: "     << outCh << " ch"
         << "  |  Latency: " << juce::String(latMs,    1) << " ms"
         << "  |  Pos: "     << juce::String(posBeats, 3) << " beats"
         << "  |  "          << juce::String(bpm, 1)      << " BPM"
         << "  |  Vocal: "   << vtLabel;

    if (engine_.getVocalTrack().hasRecordingError())
        info << "  |  Recording/playback error";
    else
    {
        switch (engine_.getVocalTrack().getWorkerStatus())
        {
            case VocalTrack::WorkerStatus::ImportPending:   info << "  |  Importing..."; break;
            case VocalTrack::WorkerStatus::ImportSucceeded: info << "  |  Import ready"; break;
            case VocalTrack::WorkerStatus::ImportFailed:    info << "  |  Import failed"; break;
            case VocalTrack::WorkerStatus::ExportPending:   info << "  |  Exporting..."; break;
            case VocalTrack::WorkerStatus::ExportSucceeded: info << "  |  Export complete"; break;
            case VocalTrack::WorkerStatus::ExportFailed:    info << "  |  Export failed"; break;
            case VocalTrack::WorkerStatus::AutosaveSucceeded: info << "  |  Autosaved"; break;
            case VocalTrack::WorkerStatus::AutosaveFailed:  info << "  |  Autosave failed"; break;
            case VocalTrack::WorkerStatus::RecoverySucceeded: info << "  |  Recovery loaded"; break;
            case VocalTrack::WorkerStatus::RecoveryFailed:  info << "  |  Recovery failed"; break;
            default:
                if (engine_.getVocalTrack().hasPlayback()) info << "  |  Latest take ready";
                break;
        }
    }

    infoLabel_.setText(info, juce::dontSendNotification);
}

} // namespace ssbb
