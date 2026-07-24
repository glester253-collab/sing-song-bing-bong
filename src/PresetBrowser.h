#pragma once
// PresetBrowser.h — project template and parameter preset management.
//
// MESSAGE THREAD ONLY.
//
// A Preset stores a named collection of parameter values.
// Templates are read-only presets shipped with the application.
// User presets are stored in the app data directory as JSON files.
//
// The PresetBrowser:
//   - Scans a directory for *.preset.json files.
//   - Loads a preset into a flat key-value map.
//   - Saves the current parameter state as a new preset.
//   - Applies a preset via a callback so the caller can update its parameters.

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ssbb {

// ---- Preset data model ---------------------------------------------------

struct Preset
{
    std::string                              name;
    std::string                              category;   ///< e.g. "Vocal", "Drum"
    std::unordered_map<std::string, float>   values;     ///< paramId → value
    bool                                     readOnly { false };
};

// ---- PresetBrowser -------------------------------------------------------

class PresetBrowser
{
public:
    /// Callback: apply `preset.values` to the engine/processor.
    using ApplyCallback = std::function<void(const Preset&)>;
    /// Callback: fill a Preset with the current parameter state.
    using CaptureCallback = std::function<void(Preset&)>;

    PresetBrowser() = default;

    // ---- Directory management --------------------------------------------

    void setUserPresetsDir(const std::filesystem::path& dir)
    {
        userDir_ = dir;
        std::filesystem::create_directories(dir);
    }

    void setFactoryPresetsDir(const std::filesystem::path& dir)
    {
        factoryDir_ = dir;
    }

    // ---- Scanning --------------------------------------------------------

    /// Rescan both factory and user directories.
    void rescan()
    {
        presets_.clear();

        scanDir(factoryDir_, /*readOnly=*/true);
        scanDir(userDir_,    /*readOnly=*/false);
    }

    const std::vector<Preset>& getPresets() const noexcept { return presets_; }

    std::vector<const Preset*> getPresetsForCategory(const std::string& cat) const
    {
        std::vector<const Preset*> out;
        for (const auto& p : presets_)
            if (p.category == cat) out.push_back(&p);
        return out;
    }

    // ---- Apply -----------------------------------------------------------

    /// Apply the preset at `index` by calling `cb`.
    bool applyPreset(std::size_t index, const ApplyCallback& cb) const
    {
        if (index >= presets_.size() || !cb) return false;
        cb(presets_[index]);
        return true;
    }

    bool applyPreset(const std::string& name, const ApplyCallback& cb) const
    {
        for (const auto& p : presets_)
        {
            if (p.name == name) { cb(p); return true; }
        }
        return false;
    }

    // ---- Save ------------------------------------------------------------

    /// Capture current state via `captureFn` and save as `name.preset.json`.
    bool saveUserPreset(const std::string& name,
                        const std::string& category,
                        const CaptureCallback& captureFn)
    {
        if (!captureFn || userDir_.empty()) return false;

        Preset p;
        p.name     = name;
        p.category = category;
        p.readOnly = false;
        captureFn(p);

        const auto path = userDir_ / (sanitize(name) + ".preset.json");
        if (!writePreset(path, p)) return false;

        // Refresh in-memory list.
        rescan();
        return true;
    }

    // ---- Delete ----------------------------------------------------------

    bool deleteUserPreset(const std::string& name)
    {
        const auto path = userDir_ / (sanitize(name) + ".preset.json");
        std::error_code ec;
        const bool ok = std::filesystem::remove(path, ec);
        if (ok) rescan();
        return ok;
    }

private:
    std::filesystem::path factoryDir_;
    std::filesystem::path userDir_;
    std::vector<Preset>   presets_;

    void scanDir(const std::filesystem::path& dir, bool readOnly)
    {
        if (dir.empty() || !std::filesystem::exists(dir)) return;

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file()) continue;
            const auto& p = entry.path();
            if (p.extension() != ".json") continue;
            if (p.stem().extension() != ".preset") continue;

            Preset preset;
            if (readPreset(p, preset))
            {
                preset.readOnly = readOnly;
                presets_.push_back(std::move(preset));
            }
        }
    }

    static bool readPreset(const std::filesystem::path& path, Preset& out)
    {
        std::ifstream f(path);
        if (!f.is_open()) return false;

        std::string line;
        bool inValues = false;
        while (std::getline(f, line))
        {
            if (line.find("\"name\":") != std::string::npos)
                out.name = parseStringField(line, "\"name\":");
            if (line.find("\"category\":") != std::string::npos)
                out.category = parseStringField(line, "\"category\":");
            if (line.find("\"values\":") != std::string::npos)
                inValues = true;
            if (inValues && line.find("\":") != std::string::npos)
            {
                const auto colon = line.find("\":");
                if (colon != std::string::npos)
                {
                    const auto q1 = line.find('"');
                    if (q1 != std::string::npos && q1 < colon)
                    {
                        const std::string key = line.substr(q1 + 1, colon - q1 - 1);
                        try {
                            const float val = std::stof(line.substr(colon + 2));
                            out.values[key] = val;
                        } catch (...) {}
                    }
                }
            }
        }
        return !out.name.empty();
    }

    static bool writePreset(const std::filesystem::path& path, const Preset& p)
    {
        std::ofstream f(path);
        if (!f.is_open()) return false;

        f << "{\n"
          << "  \"name\": \"" << jsonEscape(p.name) << "\",\n"
          << "  \"category\": \"" << jsonEscape(p.category) << "\",\n"
          << "  \"values\": {\n";

        std::size_t idx = 0;
        for (const auto& [k, v] : p.values)
        {
            f << "    \"" << jsonEscape(k) << "\": " << v;
            if (++idx < p.values.size()) f << ",";
            f << "\n";
        }

        f << "  }\n}\n";
        return f.good();
    }

    static std::string sanitize(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                out += c;
            else
                out += '_';
        }
        return out;
    }

    static std::string jsonEscape(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else            out += c;
        }
        return out;
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
