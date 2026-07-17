#include "PresetManager.h"
namespace ssbb {
bool PresetManager::save(const juce::File& file) const { if (auto xml = state_.tree().createXml()) return file.replaceWithText(xml->toString()); return false; }
bool PresetManager::load(const juce::File& file) { if (auto xml = juce::parseXML(file.loadFileAsString())) { auto restored = juce::ValueTree::fromXml(*xml); if (restored.isValid()) { state_.tree() = restored; return true; } } return false; }
} // namespace ssbb
