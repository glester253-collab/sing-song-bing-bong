#pragma once
#include "MasteringDSPChain.h"
#include <juce_gui_basics/juce_gui_basics.h>
namespace ssbb {
class MasteringPanel : public juce::Component
{
public:
    explicit MasteringPanel(MasteringDSPChain&); void paint(juce::Graphics&) override; void resized() override;
private:
    MasteringDSPChain& chain_; juce::Label title_, loudLabel_, widthLabel_, warmthLabel_; juce::Slider loud_, width_, warmth_;
};
} // namespace ssbb
