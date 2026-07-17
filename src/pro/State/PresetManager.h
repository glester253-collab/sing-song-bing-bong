#pragma once
#include "AppState.h"
#include <juce_core/juce_core.h>
namespace ssbb {
class PresetManager
{
public:
    explicit PresetManager(AppState& state) : state_(state) {}
    bool save(const juce::File& file) const;
    bool load(const juce::File& file);
private: AppState& state_;
};
} // namespace ssbb
