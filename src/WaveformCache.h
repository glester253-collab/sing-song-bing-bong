#pragma once
// WaveformCache.h — per-take peak/RMS overview computed on a worker thread.
//
// WORKER THREAD: buildFromFile() reads and analyses a WAV file.
// ANY THREAD:    isReady() and getFrames() may be read once isReady() == true.
//
// buildFromFile() sets isReady_ to false at entry and to true only after the
// frame vector is fully populated, so readers that poll isReady() always see
// a consistent (complete or absent) cache.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ssbb {

class WaveformCache
{
public:
    /// One display pixel's worth of audio overview data.
    struct Frame
    {
        float peakPos { 0.0f };   ///< Maximum positive sample value in window
        float peakNeg { 0.0f };   ///< Minimum (most negative) sample value in window
        float rms     { 0.0f };   ///< Root-mean-square of samples in window
    };

    WaveformCache()  = default;

    // Non-copyable
    WaveformCache(const WaveformCache&)            = delete;
    WaveformCache& operator=(const WaveformCache&) = delete;

    // ---- Worker-thread API -----------------------------------------------

    /// Analyse `wavPath` and build the frame overview.
    /// `samplesPerPixel` controls the time resolution (e.g. 256 at 44.1 kHz
    /// gives ~170 pixels/second).
    /// Supports our own IEEE float-32 WAV files and standard PCM int-16 WAV.
    /// WORKER THREAD ONLY.
    void buildFromFile(const std::filesystem::path& wavPath,
                       int samplesPerPixel = 256);

    // ---- Any-thread queries (safe after isReady() == true) ---------------

    /// True once buildFromFile() has completed successfully.
    [[nodiscard]]
    bool isReady() const noexcept
    {
        return isReady_.load(std::memory_order_acquire);
    }

    /// The computed frame overview.  Only meaningful when isReady() == true.
    [[nodiscard]]
    const std::vector<Frame>& getFrames() const noexcept { return frames_; }

    /// Discard previous results (e.g. before re-analysing a take).
    void reset() noexcept
    {
        isReady_.store(false, std::memory_order_release);
        frames_.clear();
    }

private:
    std::vector<Frame>    frames_;
    std::atomic<bool>     isReady_ { false };
};

} // namespace ssbb
