#include "MainComponent.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>

namespace ssbb {
class MainComponent::VocalGenerationJob final : public juce::ThreadPoolJob
{
public:
    VocalGenerationJob(MainComponent& owner, juce::String text, float energy)
        : ThreadPoolJob("Generate vocal"), owner_(owner), text_(std::move(text)), energy_(energy) {}
    JobStatus runJob() override
    {
        auto audio = owner_.ai_.synthesizeFromText(text_.toStdString(), "Verse", "Original", static_cast<float>(owner_.engine_.getTransport().getTempo()), energy_);
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(&owner_), audio = std::move(audio)]() mutable { if (safe != nullptr) safe->acceptGeneratedVocal(std::move(audio)); });
        return jobHasFinished;
    }
private:
    MainComponent& owner_; juce::String text_; float energy_;
};

class MainComponent::ExportJob final : public juce::ThreadPoolJob
{
public:
    ExportJob(MainComponent& owner, juce::File file, SongStructure song, std::vector<float> vocal, double bpm, std::array<std::array<bool,16>,3> pattern)
        : ThreadPoolJob("Export song"), owner_(owner), file_(std::move(file)), song_(std::move(song)), vocal_(std::move(vocal)), bpm_(bpm), pattern_(pattern) {}
    JobStatus runJob() override
    {
        constexpr double sr = 48000.0; int bars = 0; for (const auto& s : song_.sections) bars += s.bars;
        const int64_t total = std::max<int64_t>(1, static_cast<int64_t>(bars * 4.0 * 60.0 / std::max(50.0, bpm_) * sr));
        const auto instrumentFile = file_.getSiblingFile(file_.getFileNameWithoutExtension()+"-instruments.wav");
        const auto vocalFile = file_.getSiblingFile(file_.getFileNameWithoutExtension()+"-vocals.wav");
        auto mixWriter = openWriter(file_), beatWriter = openWriter(instrumentFile), vocalWriter = openWriter(vocalFile);
        if (mixWriter == nullptr || beatWriter == nullptr || vocalWriter == nullptr) return finish(false);
        juce::AudioBuffer<float> mix(2, 1024), beats(2, 1024), vocals(2, 1024);
        BeatEngine offlineBeat; offlineBeat.prepare(sr, 1024); offlineBeat.setTempo(bpm_); for (int l=0;l<3;++l) for(int s=0;s<16;++s) offlineBeat.setStep(l,s,pattern_[l][s]);
        MasteringDSPChain master; master.prepare({sr,1024,2});
        bool ok = true;
        for (int64_t pos = 0; pos < total && ok && !shouldExit(); pos += 1024)
        {
            const int count = static_cast<int>(std::min<int64_t>(1024, total-pos)); beats.clear(); vocals.clear(); mix.clear();
            float* ptrs[] { beats.getWritePointer(0), beats.getWritePointer(1) }; offlineBeat.render(ptrs,2,count);
            if (!vocal_.empty()) for (int i=0;i<count;++i) for(int ch=0;ch<2;++ch) vocals.setSample(ch,i,vocal_[static_cast<size_t>(pos+i)%vocal_.size()]);
            for(int ch=0;ch<2;++ch){mix.copyFrom(ch,0,beats,ch,0,count);mix.addFrom(ch,0,vocals,ch,0,count);} juce::AudioBuffer<float> mixView(mix.getArrayOfWritePointers(),2,count); master.process(mixView);
            ok = beatWriter->writeFromAudioSampleBuffer(beats,0,count) && vocalWriter->writeFromAudioSampleBuffer(vocals,0,count) && mixWriter->writeFromAudioSampleBuffer(mix,0,count);
        }
        return finish(ok && !shouldExit());
    }
private:
    JobStatus finish(bool ok)
    {
        juce::MessageManager::callAsync([safe=juce::Component::SafePointer<MainComponent>(&owner_),ok]{if(safe!=nullptr)safe->vocalPanel_.setStatus(ok?"Export complete: mix + two stems":"Export failed or cancelled");}); return jobHasFinished;
    }
    static std::unique_ptr<juce::AudioFormatWriter> openWriter(const juce::File& file)
    {
        file.deleteFile(); std::unique_ptr<juce::OutputStream> stream=file.createOutputStream(); if(stream==nullptr)return {};
        juce::WavAudioFormat format; const auto options=juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(2).withBitsPerSample(24);
        return format.createWriterFor(stream,options);
    }
    MainComponent& owner_; juce::File file_; SongStructure song_; std::vector<float> vocal_; double bpm_; std::array<std::array<bool,16>,3> pattern_;
};

MainComponent::MainComponent(AudioEngine& engine)
    : engine_(engine), song_(wizard_.createClassicRap()), beatPanel_(engine.getBeatEngine()), masteringPanel_(engine.getMasteringChain()),
      deviceSelector_(engine.getDeviceManager(),0,2,0,2,false,false,true,false)
{
    for(auto*c:{static_cast<juce::Component*>(&transportBar_),&wizardPanel_,&beatPanel_,&vocalPanel_,&masteringPanel_,&deviceHeading_,&deviceSelector_})addAndMakeVisible(c);
    deviceHeading_.setText("Audio device and recording setup",juce::dontSendNotification);deviceHeading_.setFont(juce::FontOptions(15.0f,juce::Font::bold));
    transportBar_.onPlay=[this]{engine_.getTransport().play();}; transportBar_.onStop=[this]{engine_.getTransport().stop();}; transportBar_.onRecord=[this]{startOrStopRecording();}; transportBar_.onExport=[this]{requestExport();};
    wizardPanel_.onApply=[this](int verse,int hook,int count){song_=wizard_.createClassicRap(verse,hook,count);vocalPanel_.setStatus("Song structure updated: "+juce::String(static_cast<int>(song_.sections.size()))+" sections");};
    vocalPanel_.onGenerate=[this](const juce::String& text,float energy){if(text.trim().isEmpty()){vocalPanel_.setStatus("Enter original lyrics first");return;}vocalPanel_.setStatus("Generating on worker thread...");workerPool_.addJob(new VocalGenerationJob(*this,text,energy),true);};
    vocalPanel_.onRecord=[this]{startOrStopRecording();};
    const auto models=juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("models"); ai_.loadModels(models.getFullPathName().toStdString());
    engine_.getTransport().setTempo(96.0); startTimerHz(20); setSize(1400,900);
}
MainComponent::~MainComponent(){stopTimer();workerPool_.removeAllJobs(true,5000);}
void MainComponent::acceptGeneratedVocal(std::vector<float> audio){lastGenerated_=std::move(audio);engine_.loadGeneratedVocal(lastGenerated_);if(!song_.sections.empty()){auto it=std::find_if(song_.sections.begin(),song_.sections.end(),[](const SongSection&s){return s.type==SectionType::Verse;});if(it!=song_.sections.end())it->audio=lastGenerated_;}vocalPanel_.setStatus(lastGenerated_.empty()?"Generation returned no audio":"Generated vocal ready; press Play");}
void MainComponent::startOrStopRecording(){auto&v=engine_.getVocalTrack();const auto state=v.getState();if(state==VocalTrack::State::Recording||state==VocalTrack::State::Stopping)v.stopRecording();else{if(state==VocalTrack::State::Idle)v.arm();v.startRecording();}}
void MainComponent::requestExport(){chooser_=std::make_unique<juce::FileChooser>("Export full song and stems",juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("sing-song-bing-bong.wav"),"*.wav");chooser_->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles,[this](const juce::FileChooser&fc){auto file=fc.getResult();if(file==juce::File{})return;if(file.getFileExtension().isEmpty())file=file.withFileExtension(".wav");std::array<std::array<bool,16>,3> p{};for(int l=0;l<3;++l)for(int s=0;s<16;++s)p[l][s]=engine_.getBeatEngine().getStep(l,s);vocalPanel_.setStatus("Exporting on worker thread...");workerPool_.addJob(new ExportJob(*this,file,song_,lastGenerated_,engine_.getTransport().getTempo(),p),true);});}
void MainComponent::timerCallback(){const bool playing=engine_.getTransport().isPlaying();const auto state=engine_.getVocalTrack().getState();transportBar_.setPlaying(playing);transportBar_.setRecording(state==VocalTrack::State::Recording||state==VocalTrack::State::Stopping);}
void MainComponent::paint(juce::Graphics&g){g.fillAll(juce::Colour(0xff10131b));}
void MainComponent::resized(){auto r=getLocalBounds().reduced(8);transportBar_.setBounds(r.removeFromTop(54));r.removeFromTop(6);auto top=r.removeFromTop(std::min(430,r.getHeight()/2));const int third=top.getWidth()/3;wizardPanel_.setBounds(top.removeFromLeft(third).reduced(3));beatPanel_.setBounds(top.removeFromLeft(third).reduced(3));vocalPanel_.setBounds(top.reduced(3));r.removeFromTop(6);masteringPanel_.setBounds(r.removeFromTop(72));deviceHeading_.setBounds(r.removeFromTop(28));deviceSelector_.setBounds(r);}
} // namespace ssbb
