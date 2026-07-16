#pragma once
// TakeManager.h — generates unique timestamped take file paths and stores
// take metadata.
//
// MESSAGE THREAD ONLY.  None of these methods are safe to call from the
// audio thread or concurrently without external locking.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ssbb {

/// Metadata recorded when a take path is reserved.
struct TakeMetadata
{
    std::filesystem::path path;
    double                sampleRate   { 44100.0 };
    int                   numChannels  { 1 };
    std::string           isoTimestamp;   // "YYYYMMDDTHHmmss"
};

/// Generates collision-free take paths of the form
///   <dir>/take_YYYYMMDDTHHmmss_NNN.wav
/// where NNN is a zero-padded 3-digit counter that increments until the path
/// is not already reserved in memory or present on disk.
class TakeManager
{
public:
    TakeManager()  = default;

    // Non-copyable
    TakeManager(const TakeManager&)            = delete;
    TakeManager& operator=(const TakeManager&) = delete;

    /// Reserve and return a unique path inside `dir`.
    /// Creates `dir` if it does not exist.
    /// Thread-safety: MESSAGE THREAD ONLY.
    [[nodiscard]]
    std::filesystem::path createTakePath(const std::filesystem::path& dir,
                                         double sampleRate,
                                         int    numChannels);

    /// All takes reserved so far (in reservation order).
    const std::vector<TakeMetadata>& takes() const noexcept { return takes_; }

    /// Remove all in-memory take records (does not delete files on disk).
    void clearRecords() noexcept { takes_.clear(); reserved_.clear(); }

private:
    static std::string makeTimestamp() noexcept;  // "YYYYMMDDTHHmmss"

    std::vector<TakeMetadata>          takes_;
    std::vector<std::filesystem::path> reserved_;  // paths already returned
};

} // namespace ssbb
