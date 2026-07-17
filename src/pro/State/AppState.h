#pragma once
#include <juce_data_structures/juce_data_structures.h>
namespace ssbb {
class AppState
{
public:
    AppState();
    juce::ValueTree& tree() noexcept { return tree_; }
    double tempo() const;
    void setTempo(double bpm);
private:
    juce::ValueTree tree_ { "PROState" };
};
} // namespace ssbb
