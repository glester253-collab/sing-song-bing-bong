#pragma once
// ControllerMap.h — MIDI controller mapping and MIDI-learn.
//
// MESSAGE THREAD ONLY.  All methods run on the message thread.
// MIDI events arrive on the audio thread and are forwarded to the message
// thread via a SPSC queue before being dispatched here.
//
// Design:
//   - A Mapping associates a CC number (0–127) and MIDI channel (0 = omni)
//     with a parameter ID (matching AutomationLane / VocalChain parameter names).
//   - setLearning(paramId) enters MIDI-learn mode for one parameter.
//     The next CC received is assigned to that parameter.
//   - Mappings are saved/loaded as JSON (simple key-value pairs).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ssbb {

// ---- Mapping descriptor -------------------------------------------------

struct ControllerMapping
{
    int         cc      { 0 };   ///< MIDI CC number (0–127).
    int         channel { 0 };   ///< MIDI channel (1–16); 0 = omni.
    std::string paramId;         ///< Target parameter ID.
    float       minValue{ 0.0f };///< Parameter range minimum.
    float       maxValue{ 1.0f };///< Parameter range maximum.
};

// ---- ControllerMap -------------------------------------------------------

class ControllerMap
{
public:
    /// Callback invoked when a mapped CC changes a parameter value.
    using ChangeCallback = std::function<void(const std::string& paramId, float value)>;

    ControllerMap() = default;

    // ---- Configuration ---------------------------------------------------

    void addMapping(const ControllerMapping& m)
    {
        removeMapping(m.paramId);   // one param per ID
        mappings_.push_back(m);
    }

    void removeMapping(const std::string& paramId)
    {
        mappings_.erase(
            std::remove_if(mappings_.begin(), mappings_.end(),
                [&](const ControllerMapping& m) { return m.paramId == paramId; }),
            mappings_.end());
    }

    void clearMappings() { mappings_.clear(); }

    const std::vector<ControllerMapping>& getMappings() const noexcept
    {
        return mappings_;
    }

    // ---- MIDI-learn mode ------------------------------------------------

    /// Enter MIDI-learn mode for `paramId`.  The next CC message received
    /// via processCCEvent() will be assigned to this parameter.
    void startLearning(const std::string& paramId,
                       float minValue = 0.0f, float maxValue = 1.0f)
    {
        learnParamId_  = paramId;
        learnMinValue_ = minValue;
        learnMaxValue_ = maxValue;
        learning_      = true;
    }

    /// Cancel MIDI-learn mode without assigning a mapping.
    void cancelLearning() noexcept { learning_ = false; learnParamId_.clear(); }

    bool isLearning() const noexcept { return learning_; }

    const std::string& learningParamId() const noexcept { return learnParamId_; }

    // ---- Event dispatch (message thread, from SPSC queue drain) ----------

    /// Register a callback invoked when a mapped CC fires.
    void setChangeCallback(ChangeCallback cb) { changeCb_ = std::move(cb); }

    /// Process one CC event.  In MIDI-learn mode, assigns the CC to the
    /// pending parameter and exits learn mode.
    void processCCEvent(int cc, int value, int midiChannel)
    {
        if (cc < 0 || cc > 127 || value < 0 || value > 127) return;

        if (learning_)
        {
            ControllerMapping m;
            m.cc       = cc;
            m.channel  = midiChannel;
            m.paramId  = learnParamId_;
            m.minValue = learnMinValue_;
            m.maxValue = learnMaxValue_;
            addMapping(m);
            learning_ = false;
            learnParamId_.clear();
        }

        // Dispatch to all matching mappings.
        const float norm = static_cast<float>(value) / 127.0f;
        for (const auto& m : mappings_)
        {
            if (m.cc != cc) continue;
            if (m.channel != 0 && m.channel != midiChannel) continue;
            const float paramVal = m.minValue + norm * (m.maxValue - m.minValue);
            if (changeCb_) changeCb_(m.paramId, paramVal);
        }
    }

    // ---- Persistence (JSON key-value) ------------------------------------

    bool save(const std::filesystem::path& path) const
    {
        std::ofstream f(path);
        if (!f.is_open()) return false;

        f << "{\n  \"mappings\": [\n";
        for (std::size_t i = 0; i < mappings_.size(); ++i)
        {
            const auto& m = mappings_[i];
            f << "    {\"cc\":" << m.cc
              << ",\"channel\":" << m.channel
              << ",\"paramId\":\"" << jsonEscape(m.paramId) << "\""
              << ",\"minValue\":" << m.minValue
              << ",\"maxValue\":" << m.maxValue
              << "}";
            if (i + 1 < mappings_.size()) f << ",";
            f << "\n";
        }
        f << "  ]\n}\n";
        return f.good();
    }

    bool load(const std::filesystem::path& path)
    {
        std::ifstream f(path);
        if (!f.is_open()) return false;

        // Minimal line-by-line JSON parser for our own format.
        mappings_.clear();
        std::string line;
        ControllerMapping cur;
        bool inMapping = false;
        while (std::getline(f, line))
        {
            if (line.find("\"cc\":") != std::string::npos)
            {
                cur = ControllerMapping{};
                inMapping = true;
                cur.cc = parseIntField(line, "\"cc\":");
            }
            if (inMapping)
            {
                if (line.find("\"channel\":") != std::string::npos)
                    cur.channel = parseIntField(line, "\"channel\":");
                if (line.find("\"paramId\":") != std::string::npos)
                    cur.paramId = parseStringField(line, "\"paramId\":");
                if (line.find("\"minValue\":") != std::string::npos)
                    cur.minValue = parseFloatField(line, "\"minValue\":");
                if (line.find("\"maxValue\":") != std::string::npos)
                {
                    cur.maxValue = parseFloatField(line, "\"maxValue\":");
                    mappings_.push_back(cur);
                    inMapping = false;
                }
            }
        }
        return true;
    }

private:
    std::vector<ControllerMapping> mappings_;
    ChangeCallback                 changeCb_;

    bool        learning_      { false };
    std::string learnParamId_;
    float       learnMinValue_ { 0.0f };
    float       learnMaxValue_ { 1.0f };

    // ---- Minimal JSON helpers -------------------------------------------

    static std::string jsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            if (c == '"')  { out += "\\\""; }
            else if (c == '\\') { out += "\\\\"; }
            else            out += c;
        }
        return out;
    }

    static int parseIntField(const std::string& line, const std::string& key)
    {
        const auto pos = line.find(key);
        if (pos == std::string::npos) return 0;
        return std::stoi(line.substr(pos + key.size()));
    }

    static float parseFloatField(const std::string& line, const std::string& key)
    {
        const auto pos = line.find(key);
        if (pos == std::string::npos) return 0.0f;
        return std::stof(line.substr(pos + key.size()));
    }

    static std::string parseStringField(const std::string& line, const std::string& key)
    {
        const auto pos = line.find(key);
        if (pos == std::string::npos) return {};
        const auto q1 = line.find('"', pos + key.size());
        if (q1 == std::string::npos) return {};
        const auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return line.substr(q1 + 1, q2 - q1 - 1);
    }
};

} // namespace ssbb
