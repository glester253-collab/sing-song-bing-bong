#pragma once
#include "BeatEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
namespace ssbb {
class BeatMakerPanel : public juce::Component
{
public:
    explicit BeatMakerPanel(BeatEngine&); void paint(juce::Graphics&) override; void resized() override; void sync();
private:
    BeatEngine& engine_; juce::Label title_, bpmLabel_; juce::ComboBox preset_; juce::Slider bpm_; std::array<juce::Label,3> labels_; std::array<std::array<juce::ToggleButton,16>,3> steps_;
};
} // namespace ssbb
