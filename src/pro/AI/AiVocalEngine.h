#pragma once
#include <memory>
#include <string>
#include <vector>

class AiVocalEngine
{
public:
    AiVocalEngine();
    ~AiVocalEngine();
    bool loadModels(const std::string& modelDir);
    std::vector<float> synthesizeFromText(const std::string& text, const std::string& sectionType, const std::string& stylePreset, float tempo, float energy);
    bool modelsLoaded() const noexcept;
    const std::string& getLastError() const noexcept { return lastError_; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string lastError_;
    static std::string normalize(const std::string& text);
    static std::vector<int64_t> tokenize(const std::string& text);
    static std::vector<float> makePreview(const std::string& text, float tempo, float energy);
};
