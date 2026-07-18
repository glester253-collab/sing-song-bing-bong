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

    /// Migrate `data` in-place from its current `data.version` to `toVersion`.
    ///
    /// Rules:
    ///  - If `data.version == toVersion` the function is a no-op and returns true.
    ///  - If `data.version > toVersion` (downgrade) the function returns false —
    ///    older code cannot safely interpret a newer session.
    ///  - Each version step is applied in order so a single call always brings
    ///    the data up to date regardless of how many versions behind it is.
    ///  - `toVersion` defaults to `kSchemaVersion` (current).
    ///
    /// Returns false only when downgrade is requested or an unknown version is
    /// encountered; true otherwise (including the no-op case).
    static bool migrate(SessionData& data,
                        int          toVersion = kSchemaVersion);

private:
    SessionDocument() = delete;
};

} // namespace ssbb
