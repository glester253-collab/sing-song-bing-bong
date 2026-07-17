#include "AiVocalEngine.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <numbers>
#if SSBB_ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

struct AiVocalEngine::Impl
{
#if SSBB_ENABLE_ONNX
    Ort::Env env { ORT_LOGGING_LEVEL_WARNING, "ssbb-vocal" };
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> acoustic, vocoder;
#endif
    bool ready = false;
};
AiVocalEngine::AiVocalEngine() : impl_(std::make_unique<Impl>()) {}
AiVocalEngine::~AiVocalEngine() = default;
bool AiVocalEngine::modelsLoaded() const noexcept { return impl_->ready; }
bool AiVocalEngine::loadModels(const std::string& modelDir)
{
    impl_->ready = false; const auto acoustic = std::filesystem::path(modelDir) / "acoustic.onnx"; const auto vocoder = std::filesystem::path(modelDir) / "vocoder.onnx";
    if (!exists(acoustic) || !exists(vocoder)) { lastError_ = "No licensed model pair found; preview synthesizer is active."; return false; }
#if SSBB_ENABLE_ONNX
    try { impl_->options.SetIntraOpNumThreads(1); impl_->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#if defined(_WIN32)
        impl_->acoustic = std::make_unique<Ort::Session>(impl_->env, acoustic.wstring().c_str(), impl_->options); impl_->vocoder = std::make_unique<Ort::Session>(impl_->env, vocoder.wstring().c_str(), impl_->options);
#else
        impl_->acoustic = std::make_unique<Ort::Session>(impl_->env, acoustic.c_str(), impl_->options); impl_->vocoder = std::make_unique<Ort::Session>(impl_->env, vocoder.c_str(), impl_->options);
#endif
        impl_->ready = true; lastError_.clear(); return true; } catch (const Ort::Exception& e) { lastError_ = e.what(); return false; }
#else
    lastError_ = "Models found, but ONNX Runtime is disabled in this build."; return false;
#endif
}
std::string AiVocalEngine::normalize(const std::string& text) { std::string out; bool space = true; for (unsigned char c : text) { if (std::isalnum(c) || c == '\'') { out.push_back(static_cast<char>(std::tolower(c))); space = false; } else if (!space) { out.push_back(' '); space = true; } } if (!out.empty() && out.back() == ' ') out.pop_back(); return out; }
std::vector<int64_t> AiVocalEngine::tokenize(const std::string& text) { std::vector<int64_t> out { 1 }; for (unsigned char c : text) out.push_back(static_cast<int64_t>(c) + 3); out.push_back(2); return out; }
std::vector<float> AiVocalEngine::makePreview(const std::string& text, float tempo, float energy)
{
    constexpr double sr = 48000.0; const size_t words = std::max<size_t>(1, static_cast<size_t>(std::count(text.begin(), text.end(), ' ') + 1)); const double wordSeconds = 30.0 / std::clamp<double>(tempo, 50.0, 220.0); std::vector<float> out(static_cast<size_t>(sr * wordSeconds * words)); const float gain = 0.05f + 0.11f * std::clamp(energy, 0.0f, 1.0f);
    for (size_t i = 0; i < out.size(); ++i) { const double t = i / sr, phase = std::fmod(t, wordSeconds) / wordSeconds; const unsigned char seed = text.empty() ? 97 : static_cast<unsigned char>(text[(i / static_cast<size_t>(sr * wordSeconds)) % text.size()]); const double f = 105.0 + (seed % 18) * 5.5; const float env = static_cast<float>(std::sin(std::numbers::pi * std::clamp(phase * 1.25, 0.0, 1.0))); out[i] = gain * env * static_cast<float>(std::sin(2.0 * std::numbers::pi * f * t) + 0.3 * std::sin(4.0 * std::numbers::pi * f * t)); }
    return out;
}
std::vector<float> AiVocalEngine::synthesizeFromText(const std::string& text, const std::string&, const std::string&, float tempo, float energy)
{
    const auto clean = normalize(text); if (clean.empty()) return {}; const auto tokens = tokenize(clean);
#if SSBB_ENABLE_ONNX
    if (impl_->ready) { try { auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault); std::array<int64_t, 2> tokenShape {1, static_cast<int64_t>(tokens.size())}; std::array<float, 2> controls {tempo, std::clamp(energy, 0.0f, 1.0f)}; std::array<int64_t, 2> controlShape {1, 2}; auto tokenTensor = Ort::Value::CreateTensor<int64_t>(memory, const_cast<int64_t*>(tokens.data()), tokens.size(), tokenShape.data(), 2); auto controlTensor = Ort::Value::CreateTensor<float>(memory, controls.data(), 2, controlShape.data(), 2); const char* inNames[] {"tokens", "controls"}; const char* featureName[] {"acoustic_features"}; std::array<Ort::Value, 2> inputs {std::move(tokenTensor), std::move(controlTensor)}; auto features = impl_->acoustic->Run(Ort::RunOptions{nullptr}, inNames, inputs.data(), 2, featureName, 1); const char* vocIn[] {"acoustic_features"}; const char* vocOut[] {"waveform"}; auto waveform = impl_->vocoder->Run(Ort::RunOptions{nullptr}, vocIn, features.data(), 1, vocOut, 1); const auto count = waveform.front().GetTensorTypeAndShapeInfo().GetElementCount(); const auto* samples = waveform.front().GetTensorData<float>(); return {samples, samples + count}; } catch (const Ort::Exception& e) { lastError_ = e.what(); } }
#endif
    return makePreview(clean, tempo, energy);
}
