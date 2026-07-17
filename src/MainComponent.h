#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AudioEngine.h"
#include "pro/AI/AiVocalEngine.h"
#include "pro/Song/SongWizard.h"
#include "pro/UI/TransportBar.h"
#include "pro/UI/SongWizardPanel.h"
#include "pro/UI/BeatMakerPanel.h"
#include "pro/UI/VocalPanel.h"
#include "pro/UI/MasteringPanel.h"

namespace ssbb {
class MainComponent final : public juce::Component, private juce::Timer
{
public:
    explicit MainComponent(AudioEngine& engine);
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class VocalGenerationJob;
    class ExportJob;
    void timerCallback() override;
    void startOrStopRecording();
    void requestExport();
    void acceptGeneratedVocal(std::vector<float> audio);

    AudioEngine& engine_;
    SongWizard wizard_;
    SongStructure song_;
    AiVocalEngine ai_;
    juce::ThreadPool workerPool_ { 2 };
    std::vector<float> lastGenerated_;

    TransportBar transportBar_;
    SongWizardPanel wizardPanel_;
    BeatMakerPanel beatPanel_;
    VocalPanel vocalPanel_;
    MasteringPanel masteringPanel_;
    juce::AudioDeviceSelectorComponent deviceSelector_;
    juce::Label deviceHeading_;
    std::unique_ptr<juce::FileChooser> chooser_;
};
} // namespace ssbb
