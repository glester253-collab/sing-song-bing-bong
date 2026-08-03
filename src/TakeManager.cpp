// TakeManager.cpp
// MESSAGE THREAD ONLY — no audio-thread rules apply here.
#include "TakeManager.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ssbb {

// ---- Private helper --------------------------------------------------------

std::string TakeManager::makeTimestamp() noexcept
{
    // Use UTC time to keep timestamps comparable across timezones.
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);

    std::tm tm {};
#if defined(_WIN32)
    // MSVC / MinGW: gmtime_s is thread-safe and preferred.
    gmtime_s(&tm, &t);
#else
    // POSIX: gmtime_r is thread-safe.
    gmtime_r(&t, &tm);
#endif

    // Format: YYYYMMDDTHHmmss  (ISO 8601 basic, 15 chars + NUL = 16).
    // Use std::ostringstream to avoid snprintf truncation warnings from
    // GCC's overly-conservative range analysis on tm_year.
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm.tm_year + 1900)
        << std::setw(2) << (tm.tm_mon  + 1)
        << std::setw(2) <<  tm.tm_mday
        << 'T'
        << std::setw(2) <<  tm.tm_hour
        << std::setw(2) <<  tm.tm_min
        << std::setw(2) <<  tm.tm_sec;
    return oss.str();
}

// ---- Public API ------------------------------------------------------------

std::filesystem::path TakeManager::createTakePath(
    const std::filesystem::path& dir,
    double sampleRate,
    int    numChannels)
{
    // Ensure the directory exists.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // (Ignore ec — if it fails, open() will fail later and we'll report then.)

    const std::string timestamp = makeTimestamp();

    // Find the lowest suffix N (starting at 1) such that neither:
    //   (a) the path was already returned by a previous call, nor
    //   (b) a file with that name exists on disk.
    // This prevents collisions when called rapidly within the same second and
    // also handles pre-existing files from previous sessions.
    std::filesystem::path candidate;
    bool foundFreePath = false;
    for (int n = 1; n <= 999; ++n)
    {
        // Build filename: take_YYYYMMDDTHHmmss_NNN.wav
        char suffix[5];
        std::snprintf(suffix, sizeof(suffix), "%03d", n);

        candidate = dir / ("take_" + timestamp + "_" + suffix + ".wav");

        // Check in-memory reservation set first (O(N) but N ≤ takes in session).
        bool alreadyReserved = false;
        for (const auto& p : reserved_)
            if (p == candidate) { alreadyReserved = true; break; }

        if (!alreadyReserved && !std::filesystem::exists(candidate))
        {
            foundFreePath = true;
            break;
        }

        // Never fall through with an existing path: WavWriter opens with
        // truncation, so doing so could destroy an earlier recording.
    }

    if (!foundFreePath)
        return {};

    // Record in both the ordered metadata list and the quick-lookup set.
    reserved_.push_back(candidate);
    takes_.push_back(TakeMetadata{candidate, sampleRate, numChannels, timestamp});

    return candidate;
}

void TakeManager::discardLastReservation(const std::filesystem::path& path) noexcept
{
    if (!takes_.empty() && !reserved_.empty() &&
        takes_.back().path == path && reserved_.back() == path)
    {
        takes_.pop_back();
        reserved_.pop_back();
    }
}

} // namespace ssbb
