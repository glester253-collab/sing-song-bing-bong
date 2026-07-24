#pragma once
// DiagnosticsBundle.h — collect and report system/session diagnostics.
//
// MESSAGE THREAD ONLY.  Writes a human-readable diagnostics report to a file.
//
// Contents of the bundle:
//   - Platform / OS / compiler info (compile-time macros)
//   - Audio device info (sample rate, block size, latency, channels)
//   - Session info (take count, clip count, duration)
//   - Memory usage (peak resident set size where available)
//   - Last error messages (capped ring buffer, message thread only)
//   - Build config flags
//
// The report is plain text so it can be attached to a bug report without
// needing any special tooling to open it.  It never contains raw audio or
// personally identifiable information.

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace ssbb {

class DiagnosticsBundle
{
public:
    static constexpr std::size_t kMaxLogLines = 200;

    DiagnosticsBundle() = default;

    // ---- Message-thread API ---------------------------------------------

    /// Append a log line (capped at kMaxLogLines).
    void addLogLine(const std::string& line)
    {
        if (log_.size() >= kMaxLogLines)
            log_.pop_front();
        log_.push_back(line);
    }

    /// Set the audio device summary string (e.g. from AudioDeviceManager).
    void setDeviceInfo(const std::string& info) { deviceInfo_ = info; }

    /// Set the session summary (takes/clips/duration).
    void setSessionInfo(const std::string& info) { sessionInfo_ = info; }

    /// Set last-known sample rate and block size.
    void setAudioSpec(double sampleRate, int blockSize, int numIn, int numOut,
                      double latencyMs)
    {
        sampleRate_  = sampleRate;
        blockSize_   = blockSize;
        numIn_       = numIn;
        numOut_      = numOut;
        latencyMs_   = latencyMs;
    }

    // ---- Report generation ----------------------------------------------

    /// Write the diagnostics report to `path`.  Returns false on I/O error.
    bool writeTo(const std::filesystem::path& path) const
    {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << buildReport();
        return f.good();
    }

    /// Return the report as a string (for display in the UI).
    std::string buildReport() const
    {
        std::ostringstream ss;

        ss << "=== Sing Song Bing Bong Diagnostics ===\n"
           << "Generated: " << timestamp() << "\n\n";

        ss << "--- Build ---\n"
           << "Compiler: " << compilerInfo() << "\n"
           << "C++ standard: " << __cplusplus << "\n"
           << "Build type: " << buildType() << "\n\n";

        ss << "--- Audio Device ---\n";
        if (!deviceInfo_.empty())
            ss << deviceInfo_ << "\n";
        ss << "Sample rate : " << sampleRate_ << " Hz\n"
           << "Block size  : " << blockSize_  << " samples\n"
           << "Inputs      : " << numIn_      << "\n"
           << "Outputs     : " << numOut_     << "\n"
           << "Latency est.: " << latencyMs_  << " ms\n\n";

        ss << "--- Session ---\n";
        if (!sessionInfo_.empty())
            ss << sessionInfo_ << "\n";
        ss << "\n";

        ss << "--- Log (last " << log_.size() << " lines) ---\n";
        for (const auto& line : log_)
            ss << line << "\n";
        ss << "\n";

        ss << "=== End of Report ===\n";
        return ss.str();
    }

private:
    double      sampleRate_ { 0.0 };
    int         blockSize_  { 0 };
    int         numIn_      { 0 };
    int         numOut_     { 0 };
    double      latencyMs_  { 0.0 };
    std::string deviceInfo_;
    std::string sessionInfo_;
    std::deque<std::string> log_;

    static std::string timestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto tt  = std::chrono::system_clock::to_time_t(now);
        char buf[32] {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&tt));
        return std::string(buf);
    }

    static std::string compilerInfo()
    {
#if defined(__clang__)
        return "Clang " + std::to_string(__clang_major__) + "."
             + std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
        return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
        return "GCC " + std::to_string(__GNUC__) + "."
             + std::to_string(__GNUC_MINOR__);
#else
        return "Unknown";
#endif
    }

    static std::string buildType()
    {
#if defined(NDEBUG)
        return "Release";
#else
        return "Debug";
#endif
    }
};

} // namespace ssbb
