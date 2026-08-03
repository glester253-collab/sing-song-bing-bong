#pragma once
// SessionDocument.h — versioned JSON session persistence.
//
// Provides save / load / saveRecovery / loadRecovery for the session data
// model.  JSON is constructed and parsed manually — no external library.
//
// MESSAGE THREAD ONLY for all functions.
// Schema version = 1.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ssbb {

// ---- Data model types -------------------------------------------------------

struct TakeEntry
{
    std::string path;
    double      sampleRate   { 44100.0 };
    int         numChannels  { 1 };
    std::string isoTimestamp;   // "YYYYMMDDTHHmmss"
};

struct ClipEntry
{
    std::string takePath;
    int64_t     offsetSamples       { 0 };
    int64_t     trimStartSamples    { 0 };
    int64_t     trimEndSamples      { 0 };
    int64_t     sourceLengthSamples { 0 };
};

struct SessionData
{
    int                    version { 1 };
    std::vector<TakeEntry> takes;
    std::vector<ClipEntry> clips;
    bool                   dirty   { false };
};

// ---- Persistence ------------------------------------------------------------

class SessionDocument
{
public:
    /// Write `data` to `path` as JSON.  Returns false on I/O failure.
    static bool save(const std::filesystem::path& path,
                     const SessionData&           data);

    /// Read JSON from `path` into `data`.  Returns false on failure.
    static bool load(const std::filesystem::path& path,
                     SessionData&                 data);

    /// Write a companion recovery file: <name>.recovery.json.
    static bool saveRecovery(const std::filesystem::path& sessionPath,
                             const SessionData&           data);

    /// Read the companion recovery file.  Returns false if it does not exist
    /// or is corrupt.
    static bool loadRecovery(const std::filesystem::path& sessionPath,
                             SessionData&                 data);

    /// Schema version this code reads/writes.
    static constexpr int kSchemaVersion = 1;

    /// Migrate `data` from an older schema version to `kSchemaVersion` in place.
    /// Returns true if migration was applied or no migration was needed.
    /// Returns false if the version is unknown / too new to migrate.
    static bool migrate(SessionData& data) noexcept;

private:
    SessionDocument() = delete;
};

} // namespace ssbb
