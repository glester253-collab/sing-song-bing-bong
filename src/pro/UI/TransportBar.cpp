#include "TransportBar.h"
namespace ssbb {
TransportBar::TransportBar() { title_.setText("sing-song-bing-bong PRO", juce::dontSendNotification); title_.setFont(juce::FontOptions(21.0f, juce::Font::bold)); addAndMakeVisible(title_); addAndMakeVisible(play_); addAndMakeVisible(stop_); addAndMakeVisible(record_); addAndMakeVisible(export_); play_.onClick=[this]{if(onPlay)onPlay();}; stop_.onClick=[this]{if(onStop)onStop();}; record_.onClick=[this]{if(onRecord)onRecord();}; export_.onClick=[this]{if(onExport)onExport();}; }
void TransportBar::resized() { auto r=getLocalBounds().reduced(6); title_.setBounds(r.removeFromLeft(300)); export_.setBounds(r.removeFromRight(155)); play_.setBounds(r.removeFromLeft(75).reduced(3)); stop_.setBounds(r.removeFromLeft(75).reduced(3)); record_.setBounds(r.removeFromLeft(90).reduced(3)); }
void TransportBar::setPlaying(bool b){play_.setColour(juce::TextButton::buttonColourId,b?juce::Colours::seagreen:juce::Colours::darkgrey);} void TransportBar::setRecording(bool b){record_.setColour(juce::TextButton::buttonColourId,b?juce::Colours::red:juce::Colours::darkred);}
} // namespace ssbb
