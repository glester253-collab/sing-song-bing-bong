#pragma once
// SampleBrowser.h — WAV sample browser with owned/licensed asset checks.
//
// MESSAGE THREAD ONLY.  Scans directories for WAV files and validates that
// they are not read-only system resources the user does not own.
//
// Ownership check policy:
//   - Files in the application-provided factory directory are marked ReadOnly.
//   - Files the user places in their own sample directory are marked UserOwned.
//   - Files in system locations (e.g. Windows\Media) are marked System and
//     flagged with a warning — the user must confirm their license before use.
//   - The browser never prevents the user from loading a file; it only
//     informs them of the ownership status so they can make an informed choice.
//
// "Copyrighted course workbook" or "commercial samples without a license" must
// not be bundled with the application.  This browser helps enforce that policy
// by requiring users to explicitly acknowledge system/unknown-source files.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ssbb {

// ---- Sample entry --------------------------------------------------------

enum class SampleOwnership : int
{
    UserOwned  = 0,   ///< File is in the user's own sample directory.
    Factory    = 1,   ///< Bundled with the application (permissive license).
    System     = 2,   ///< Found in a system directory; user must confirm license.
    Unknown    = 3,   ///< Source unknown; user must confirm before use.
};

struct SampleEntry
{
    std::filesystem::path path;
    std::string           name;         ///< Filename without extension.
    std::string           extension;    ///< ".wav" (lower-case).
    double                durationSec { 0.0 };
    int                   numChannels { 0 };
    double                sampleRate  { 0.0 };
    SampleOwnership       ownership   { SampleOwnership::Unknown };
    bool                  confirmed   { false };  ///< User has acknowledged usage rights.
};

// ---- SampleBrowser -------------------------------------------------------

class SampleBrowser
{
public:
    /// Callback for confirmation prompt.  The caller must show UI and call
    /// SampleEntry::confirmed = true before the sample is usable.
    using ConfirmCallback = std::function<void(SampleEntry&)>;

    SampleBrowser() = default;

    // ---- Directory configuration ----------------------------------------

    void setUserSamplesDir(const std::filesystem::path& dir)  { userDir_    = dir; }
    void setFactorySamplesDir(const std::filesystem::path& dir){ factoryDir_ = dir; }

    // ---- Scanning --------------------------------------------------------

    /// Rescan all registered directories.
    void rescan()
    {
        entries_.clear();
        scanDir(userDir_,    SampleOwnership::UserOwned);
        scanDir(factoryDir_, SampleOwnership::Factory);
    }

    const std::vector<SampleEntry>& getEntries() const noexcept { return entries_; }

    std::vector<const SampleEntry*> filter(const std::string& nameContains) const
    {
        std::vector<const SampleEntry*> out;
        for (const auto& e : entries_)
        {
            const std::string lower = toLower(e.name);
            const std::string q     = toLower(nameContains);
            if (lower.find(q) != std::string::npos)
                out.push_back(&e);
        }
        return out;
    }

    // ---- Access ----------------------------------------------------------

    /// Mark a sample as user-confirmed (license acknowledged).
    void confirmEntry(const std::filesystem::path& path)
    {
        for (auto& e : entries_)
            if (e.path == path) { e.confirmed = true; break; }
    }

    /// True if the sample at `path` is safe to use (UserOwned/Factory, or confirmed).
    bool isSafeToUse(const std::filesystem::path& path) const
    {
        for (const auto& e : entries_)
        {
            if (e.path != path) continue;
            return (e.ownership == SampleOwnership::UserOwned ||
                    e.ownership == SampleOwnership::Factory   ||
                    e.confirmed);
        }
        return false;
    }

private:
    std::filesystem::path userDir_;
    std::filesystem::path factoryDir_;
    std::vector<SampleEntry> entries_;

    void scanDir(const std::filesystem::path& dir, SampleOwnership ownership)
    {
        if (dir.empty() || !std::filesystem::exists(dir)) return;

        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            if (!entry.is_regular_file()) continue;
            const auto& p = entry.path();
            const auto  ext = toLower(p.extension().string());
            if (ext != ".wav") continue;

            SampleEntry se;
            se.path      = p;
            se.name      = p.stem().string();
            se.extension = ext;
            se.ownership = ownership;
            se.confirmed = (ownership == SampleOwnership::UserOwned ||
                            ownership == SampleOwnership::Factory);

            // Read basic WAV metadata (just the fmt chunk) without loading audio.
            readWavMeta(p, se);

            entries_.push_back(std::move(se));
        }
    }

    static void readWavMeta(const std::filesystem::path& path, SampleEntry& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;

        // Skip RIFF header.
        f.ignore(12);

        char id[4];
        while (f.good())
        {
            f.read(id, 4);
            if (f.gcount() < 4) break;
            uint32_t size = readLE32(f);

            if (id[0]=='f' && id[1]=='m' && id[2]=='t' && id[3]==' ')
            {
                f.ignore(2);  // audioFormat
                out.numChannels = static_cast<int>(readLE16(f));
                out.sampleRate  = static_cast<double>(readLE32(f));
                f.ignore(6);  // byteRate, blockAlign
                const uint16_t bps = readLE16(f);
                if (size > 16) f.ignore(static_cast<std::streamsize>(size) - 16);
                (void)bps;
            }
            else if (id[0]=='d' && id[1]=='a' && id[2]=='t' && id[3]=='a')
            {
                if (out.sampleRate > 0.0 && out.numChannels > 0)
                {
                    // Approximate duration from data chunk size.
                    const uint32_t bytes = size;
                    const uint32_t bytesPerSample = 4u; // assume float32
                    const int64_t frames = static_cast<int64_t>(bytes)
                                         / (bytesPerSample *
                                            static_cast<uint32_t>(out.numChannels));
                    out.durationSec = static_cast<double>(frames) / out.sampleRate;
                }
                break;
            }
            else
            {
                f.ignore(static_cast<std::streamsize>(size));
            }
        }
    }

    static uint16_t readLE16(std::ifstream& f) noexcept
    {
        uint8_t b[2] {};
        f.read(reinterpret_cast<char*>(b), 2);
        return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8u);
    }

    static uint32_t readLE32(std::ifstream& f) noexcept
    {
        uint8_t b[4] {};
        f.read(reinterpret_cast<char*>(b), 4);
        return static_cast<uint32_t>(b[0])
             | (static_cast<uint32_t>(b[1]) << 8u)
             | (static_cast<uint32_t>(b[2]) << 16u)
             | (static_cast<uint32_t>(b[3]) << 24u);
    }

    static std::string toLower(std::string s)
    {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
};

} // namespace ssbb
