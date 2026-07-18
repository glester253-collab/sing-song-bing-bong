// MainComponent.cpp
// All code here runs on the JUCE message thread.
// No audio-thread rules apply; JUCE APIs and String construction are fine.
#include "MainComponent.h"
#include "WavReader.h"
#include "WavWriter.h"

namespace ssbb {

// ---- Autosave period: every 30 seconds (20 Hz timer × 600 ticks) ---------
static constexpr int kAutosaveTicks = 600;

// ---- Constructor ----------------------------------------------------------

MainComponent::MainComponent(AudioEngine& engine)
    : engine_(engine),
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
    // ---- Locate app data directories ------------------------------------
    const juce::File appDir =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("SingSongBingBong");
    appDir.createDirectory();

    deviceSettingsFile_ = appDir.getChildFile("device_settings.xml");
    sessionFile_        = appDir.getChildFile("default_session.json");

    // Set default take directory inside app data.
    const juce::File takesDir = appDir.getChildFile("takes");
    takesDir.createDirectory();
    engine_.getVocalTrack().setTakeDirectory(takesDir.getFullPathName().toStdString());

    // ---- Restore device settings from last run ---------------------------
    loadDeviceSettings();

    // ---- Check for a crash-recovery file before building the UI ----------
    checkForRecovery();

    // ---- Device selector ------------------------------------------------
    addAndMakeVisible(deviceSelector_);

    // ---- Play / Stop ----------------------------------------------------
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

    // ---- Rewind ---------------------------------------------------------
    addAndMakeVisible(rewindButton_);
    rewindButton_.onClick = [this]
    {
        engine_.getTransport().stop();
        engine_.getTransport().setPositionInBeats(0.0);
        playStopButton_.setButtonText("Play");
    };

    // ---- Tempo ----------------------------------------------------------
    tempoLabel_.setText("BPM", juce::dontSendNotification);
    tempoLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(tempoLabel_);

    tempoSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    tempoSlider_.setRange(20.0, 300.0, 0.1);
    tempoSlider_.setValue(120.0, juce::dontSendNotification);
    tempoSlider_.setTooltip("Tempo (BPM)");
    tempoSlider_.onValueChange = [this]
    {
        engine_.getTransport().setTempo(tempoSlider_.getValue());
    };
    addAndMakeVisible(tempoSlider_);

    // ---- Time signature -------------------------------------------------
    timeSigLabel_.setText("Sig:", juce::dontSendNotification);
    timeSigLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(timeSigLabel_);

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
    addAndMakeVisible(dividerLabel_);

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

    // ---- Loop toggle ----------------------------------------------------
    loopToggle_.setToggleState(false, juce::dontSendNotification);
    loopToggle_.onClick = [this]
    {
        engine_.getTransport().setLoopEnabled(loopToggle_.getToggleState());
    };
    addAndMakeVisible(loopToggle_);

    // ---- Loop range row -------------------------------------------------
    loopRangeLabel_.setText("Loop range (beats):", juce::dontSendNotification);
    addAndMakeVisible(loopRangeLabel_);

    loopStartLabel_.setText("Start:", juce::dontSendNotification);
    addAndMakeVisible(loopStartLabel_);

    loopStartEditor_.setText("0.0", juce::dontSendNotification);
    loopStartEditor_.setInputRestrictions(10, "0123456789.");
    addAndMakeVisible(loopStartEditor_);

    loopEndLabel_.setText("End:", juce::dontSendNotification);
    addAndMakeVisible(loopEndLabel_);

    loopEndEditor_.setText("16.0", juce::dontSendNotification);
    loopEndEditor_.setInputRestrictions(10, "0123456789.");
    addAndMakeVisible(loopEndEditor_);

    loopSetButton_.onClick = [this]
    {
        const double start = loopStartEditor_.getText().getDoubleValue();
        const double end   = loopEndEditor_.getText().getDoubleValue();
        if (end > start)
        {
            engine_.getTransport().setLoopRange(start, end);
        }
    };
    addAndMakeVisible(loopSetButton_);

    // ---- Metronome row --------------------------------------------------
    metronomeToggle_.setToggleState(false, juce::dontSendNotification);
    metronomeToggle_.onClick = [this]
    {
        engine_.getMetronome().setEnabled(metronomeToggle_.getToggleState());
    };
    addAndMakeVisible(metronomeToggle_);

    metroLevelLabel_.setText("Metro level:", juce::dontSendNotification);
    addAndMakeVisible(metroLevelLabel_);

    metroLevelSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    metroLevelSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    metroLevelSlider_.setRange(0.0, 1.0, 0.01);
    metroLevelSlider_.setValue(1.0, juce::dontSendNotification);
    metroLevelSlider_.setTooltip("Metronome click level");
    metroLevelSlider_.onValueChange = [this]
    {
        engine_.getMetronome().setLevel(static_cast<float>(metroLevelSlider_.getValue()));
    };
    addAndMakeVisible(metroLevelSlider_);

    // ---- Arm ------------------------------------------------------------
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

    // ---- Monitor --------------------------------------------------------
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

    // ---- Record ---------------------------------------------------------
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

    // ---- WAV import / export --------------------------------------------
    addAndMakeVisible(importWavButton_);
    importWavButton_.onClick = [this] { importWav(); };

    addAndMakeVisible(exportWavButton_);
    exportWavButton_.onClick = [this] { exportWav(); };

    // ---- Undo / Redo ----------------------------------------------------
    addAndMakeVisible(undoButton_);
    undoButton_.onClick = [this]
    {
        commandHistory_.undo();
        undoButton_.setEnabled(commandHistory_.canUndo());
        redoButton_.setEnabled(commandHistory_.canRedo());
    };
    undoButton_.setEnabled(false);

    addAndMakeVisible(redoButton_);
    redoButton_.onClick = [this]
    {
        commandHistory_.redo();
        undoButton_.setEnabled(commandHistory_.canUndo());
        redoButton_.setEnabled(commandHistory_.canRedo());
    };
    redoButton_.setEnabled(false);

    // ---- Info label -----------------------------------------------------
    infoLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(infoLabel_);

    // Prime transport defaults.
    engine_.getTransport().setTempo(120.0);
    engine_.getTransport().setTimeSignature(4, 4);
    engine_.getTransport().setLoopRange(0.0, 16.0);

    startTimerHz(20);          // 50 ms refresh
    setSize(740, 680);
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveDeviceSettings();
    // Final autosave on clean exit.
    autosave();
}

// ---- Layout ---------------------------------------------------------------

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    // Device selector (top area)
    deviceSelector_.setBounds(bounds.removeFromTop(300));
    bounds.removeFromTop(6);

    // Transport row
    {
        auto row = bounds.removeFromTop(32);
        rewindButton_.setBounds(row.removeFromLeft(36));
        row.removeFromLeft(4);
        playStopButton_.setBounds(row.removeFromLeft(80));
        row.removeFromLeft(8);
        tempoLabel_.setBounds(row.removeFromLeft(36));
        tempoSlider_.setBounds(row.removeFromLeft(150));
        row.removeFromLeft(8);
        timeSigLabel_.setBounds(row.removeFromLeft(30));
        numeratorBox_.setBounds(row.removeFromLeft(44));
        dividerLabel_.setBounds(row.removeFromLeft(14));
        denominatorBox_.setBounds(row.removeFromLeft(44));
        row.removeFromLeft(8);
        loopToggle_.setBounds(row.removeFromLeft(56));
    }
    bounds.removeFromTop(4);

    // Loop range row
    {
        auto row = bounds.removeFromTop(28);
        loopRangeLabel_.setBounds(row.removeFromLeft(140));
        row.removeFromLeft(4);
        loopStartLabel_.setBounds(row.removeFromLeft(40));
        loopStartEditor_.setBounds(row.removeFromLeft(60));
        row.removeFromLeft(6);
        loopEndLabel_.setBounds(row.removeFromLeft(32));
        loopEndEditor_.setBounds(row.removeFromLeft(60));
        row.removeFromLeft(6);
        loopSetButton_.setBounds(row.removeFromLeft(70));
    }
    bounds.removeFromTop(4);

    // Metronome row
    {
        auto row = bounds.removeFromTop(28);
        metronomeToggle_.setBounds(row.removeFromLeft(100));
        row.removeFromLeft(8);
        metroLevelLabel_.setBounds(row.removeFromLeft(90));
        metroLevelSlider_.setBounds(row.removeFromLeft(160));
    }
    bounds.removeFromTop(4);

    // Recording row
    {
        auto row = bounds.removeFromTop(32);
        armButton_.setBounds(row.removeFromLeft(56));
        row.removeFromLeft(6);
        monitorButton_.setBounds(row.removeFromLeft(76));
        row.removeFromLeft(6);
        recordButton_.setBounds(row.removeFromLeft(90));
    }
    bounds.removeFromTop(4);

    // WAV import/export + undo/redo row
    {
        auto row = bounds.removeFromTop(32);
        importWavButton_.setBounds(row.removeFromLeft(100));
        row.removeFromLeft(6);
        exportWavButton_.setBounds(row.removeFromLeft(100));
        row.removeFromLeft(20);
        undoButton_.setBounds(row.removeFromLeft(70));
        row.removeFromLeft(6);
        redoButton_.setBounds(row.removeFromLeft(70));
    }
    bounds.removeFromTop(6);

    // Info label
    infoLabel_.setBounds(bounds.removeFromTop(24));
}

// ---- Timer (20 Hz) --------------------------------------------------------

void MainComponent::timerCallback()
{
    // Sync play/stop button label with actual transport state.
    playStopButton_.setButtonText(
        engine_.getTransport().isPlaying() ? "Stop" : "Play");

    // Sync vocal-track button states.
    const auto vtState = engine_.getVocalTrack().getState();

    armButton_.setToggleState(
        vtState != VocalTrack::State::Idle,
        juce::dontSendNotification);

    monitorButton_.setToggleState(
        vtState == VocalTrack::State::Monitoring,
        juce::dontSendNotification);

    recordButton_.setButtonText(
        (vtState == VocalTrack::State::Recording ||
         vtState == VocalTrack::State::Stopping)
        ? "Stop Rec" : "Record");

    // Autosave every kAutosaveTicks ticks (~30 s at 20 Hz).
    ++autosaveTickCount_;
    if (autosaveTickCount_ >= kAutosaveTicks)
    {
        autosaveTickCount_ = 0;
        autosave();
    }

    updateInfoLabel();
}

void MainComponent::updateInfoLabel()
{
    const int     inCh    = engine_.getNumInputChannels();
    const int     outCh   = engine_.getNumOutputChannels();
    const double  latMs   = engine_.getEstimatedLatencyMs();
    const double  bpm     = engine_.getTransport().getTempo();
    const double  sr      = engine_.getTransport().getSampleRate();
    const int64_t posSamp = engine_.getTransport().getPositionInSamples();

    const double posBeats = (bpm > 0.0 && sr > 0.0)
                                ? Transport::samplesToBeats(posSamp, bpm, sr)
                                : 0.0;

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
    info << "In: "       << inCh  << " ch"
         << "  Out: "    << outCh << " ch"
         << "  Lat: "    << juce::String(latMs,    1) << " ms"
         << "  Pos: "    << juce::String(posBeats, 2) << " beats"
         << "  SR: "     << static_cast<int>(sr)     << " Hz"
         << "  Vocal: "  << vtLabel;

    infoLabel_.setText(info, juce::dontSendNotification);
}

// ---- Device persistence --------------------------------------------------

void MainComponent::saveDeviceSettings()
{
    auto xml = engine_.getDeviceManager().createStateXml();
    if (xml != nullptr)
        xml->writeTo(deviceSettingsFile_);
}

void MainComponent::loadDeviceSettings()
{
    if (!deviceSettingsFile_.existsAsFile())
        return;

    auto xml = juce::XmlDocument::parse(deviceSettingsFile_);
    if (xml != nullptr)
    {
        // Re-initialise with the saved state.
        engine_.getDeviceManager().initialise(2, 2, xml.get(), false);
    }
}

// ---- Autosave / crash recovery -------------------------------------------

void MainComponent::autosave()
{
    if (!sessionFile_.getParentDirectory().exists())
        return;

    sessionData_.dirty = true;
    SessionDocument::saveRecovery(sessionFile_.getFullPathName().toStdString(),
                                  sessionData_);
}

void MainComponent::checkForRecovery()
{
    SessionData recovered;
    if (!SessionDocument::loadRecovery(sessionFile_.getFullPathName().toStdString(),
                                       recovered))
        return;

    if (recovered.takes.empty() && recovered.clips.empty())
        return;

    const int choice = juce::AlertWindow::showYesNoCancelBox(
        juce::AlertWindow::WarningIcon,
        "Recover Session?",
        "A recovery file was found from a previous run.\n"
        "Would you like to restore it?",
        "Restore",
        "Discard",
        "Cancel",
        this);

    if (choice == 1)   // Restore
    {
        sessionData_ = recovered;
        // The recovered takes/clips would be wired into ClipPlayer here
        // once the session loading pipeline is complete.
    }
    else if (choice == 2)   // Discard
    {
        // Remove the stale recovery file.
        juce::File rf(sessionFile_.getFullPathName() + ".recovery.json");
        // The actual recovery path uses stem + ".recovery.json":
        const auto rfPath = sessionFile_.getParentDirectory()
                                .getChildFile(sessionFile_.getFileNameWithoutExtension()
                                             + ".recovery.json");
        rfPath.deleteFile();
    }
    // choice == 0 (Cancel) → leave file in place, do nothing.
}

// ---- WAV import / export ------------------------------------------------

void MainComponent::importWav()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import WAV file",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.wav");

    chooser->launchAsync(
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this, chooser](const juce::FileChooser& fc)
        {
            const auto result = fc.getResult();
            if (!result.existsAsFile()) return;

            // Build a TakeEntry for the imported file.
            TakeEntry te;
            te.path         = result.getFullPathName().toStdString();
            te.sampleRate   = 44100.0;   // will be updated after WavReader loads
            te.numChannels  = 1;
            te.isoTimestamp = "imported";

            // Build a Clip spanning the whole file (trim = 0, offset = 0).
            // sourceLengthSamples will be set after the worker loads the file.
            Clip clip;
            clip.takePath            = result.getFullPathName().toStdString();
            clip.offsetSamples       = 0;
            clip.trimStartSamples    = 0;
            clip.trimEndSamples      = 0;
            clip.sourceLengthSamples = 0;   // placeholder

            const int slotIndex = engine_.getClipPlayer().addClip(clip);
            if (slotIndex < 0)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Import Failed",
                    "No free clip slots available (maximum 32 clips).");
                return;
            }

            // Load clip audio on the JUCE thread pool (worker thread).
            juce::Thread::launch([this, slotIndex, te, clip]() mutable
            {
                const bool ok = engine_.getClipPlayer().loadClip(slotIndex);
                if (ok)
                {
                    // Update sessionData on message thread after load.
                    juce::MessageManager::callAsync([this, te]
                    {
                        sessionData_.takes.push_back(te);
                    });
                }
                else
                {
                    juce::MessageManager::callAsync([]
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon,
                            "Import Failed",
                            "Could not read the WAV file.\n"
                            "Only IEEE float-32 and PCM int-16 WAV are supported.");
                    });
                }
            });
        });
}

void MainComponent::exportWav()
{
    // Simple mix-down: collect all loaded clip audio and write a WAV file.
    auto chooser = std::make_shared<juce::FileChooser>(
        "Export mix as WAV",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("mix.wav"),
        "*.wav");

    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser](const juce::FileChooser& fc)
        {
            const auto result = fc.getResult();
            if (result.getFullPathName().isEmpty()) return;

            const double sr = engine_.getTransport().getSampleRate();
            if (sr <= 0.0) return;

            // Determine mix length from session clips.
            int64_t totalFrames = 0;
            for (const auto& ce : sessionData_.clips)
                totalFrames = std::max(totalFrames,
                    ce.offsetSamples + ce.sourceLengthSamples
                    - ce.trimStartSamples - ce.trimEndSamples);

            if (totalFrames <= 0)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Nothing to Export",
                    "There are no clips in the session to export.");
                return;
            }

            const std::string outPath =
                result.withFileExtension("wav").getFullPathName().toStdString();

            // Render on a background thread.
            juce::Thread::launch([this, outPath, totalFrames, sr]
            {
                WavWriter writer;
                if (!writer.open(outPath, sr, /*numChannels=*/1))
                {
                    juce::MessageManager::callAsync([]
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon,
                            "Export Failed",
                            "Could not create the output file.");
                    });
                    return;
                }

                constexpr int kBlock = 1024;
                std::vector<float> buf(kBlock);
                float* chans[1] = { buf.data() };

                for (int64_t pos = 0; pos < totalFrames; pos += kBlock)
                {
                    const int frames = static_cast<int>(
                        std::min(static_cast<int64_t>(kBlock), totalFrames - pos));

                    std::fill(buf.begin(), buf.begin() + frames, 0.0f);

                    engine_.getClipPlayer().processBlock(chans, 1, frames, pos);

                    writer.write(buf.data(), frames);
                }

                writer.close();

                juce::MessageManager::callAsync([]
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::InfoIcon,
                        "Export Complete",
                        "Mix exported successfully.");
                });
            });
        });
}

} // namespace ssbb

