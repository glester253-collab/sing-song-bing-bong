#include "AppState.h"
#include <algorithm>
namespace ssbb {
AppState::AppState() { tree_.setProperty("tempo", 96.0, nullptr); tree_.setProperty("style", "West Coast Bounce", nullptr); }
double AppState::tempo() const { return static_cast<double>(tree_.getProperty("tempo", 96.0)); }
void AppState::setTempo(double bpm) { tree_.setProperty("tempo", std::clamp(bpm, 50.0, 220.0), nullptr); }
} // namespace ssbb
