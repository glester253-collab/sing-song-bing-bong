#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace ssbb {
class TransportBar : public juce::Component
{
public:
    TransportBar(); void resized() override; void setPlaying(bool); void setRecording(bool);
    std::function<void()> onPlay, onStop, onRecord, onExport;
private:
    juce::Label title_; juce::TextButton play_ {"Play"}, stop_ {"Stop"}, record_ {"Record"}, export_ {"Export WAV + stems"};
};
} // namespace ssbb
