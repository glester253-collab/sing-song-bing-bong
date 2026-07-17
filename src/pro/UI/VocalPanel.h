#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace ssbb {
class VocalPanel : public juce::Component
{
public:
    VocalPanel(); void paint(juce::Graphics&) override; void resized() override; void setStatus(const juce::String&);
    std::function<void(const juce::String&,float)> onGenerate; std::function<void()> onRecord;
private:
    juce::Label title_, energyLabel_, status_; juce::TextEditor text_; juce::Slider energy_; juce::TextButton generate_{"Generate original AI vocal"}, record_{"Record live vocal"};
};
} // namespace ssbb
