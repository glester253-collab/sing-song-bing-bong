#pragma once
#include "SongStructure.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace ssbb {
class SongWizardPanel : public juce::Component
{
public:
    SongWizardPanel(); void paint(juce::Graphics&) override; void resized() override;
    std::function<void(int,int,int)> onApply;
private:
    juce::Label title_, verseLabel_, hookLabel_, countLabel_; juce::Slider verse_, hook_, count_; juce::TextButton apply_ {"Apply Structure"};
};
} // namespace ssbb
