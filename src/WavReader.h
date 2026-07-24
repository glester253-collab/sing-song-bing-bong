#pragma once
// WavReader.h — header-only minimal WAV reader.
//
// Reads IEEE float-32 WAV files (our own recording format) and PCM int-16
// WAV files into a pre-allocated float buffer on a WORKER thread.
// The audio thread may only read from the buffer after isLoaded() == true.
//
// WORKER THREAD:  load()
// AUDIO THREAD:   isLoaded(), numFrames(), numChannels(), sampleRate(), read()
//
// The buffer is a value member (heap-allocated once in load()).  After load()
// returns, the buffer is immutable and safe to read from any thread without
// locking.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace ssbb {

class WavReader
{
public:
    WavReader() = default;

    // Non-copyable; movable.
    WavReader(const WavReader&)            = delete;
    WavReader& operator=(const WavReader&) = delete;

    WavReader(WavReader&& o) noexcept
        : samples_(std::move(o.samples_))
        , numFrames_(o.numFrames_)
        , numChannels_(o.numChannels_)
        , sampleRate_(o.sampleRate_)
    {
        isLoaded_.store(o.isLoaded_.load(std::memory_order_acquire),
                        std::memory_order_release);
        o.isLoaded_.store(false, std::memory_order_release);
        o.numFrames_   = 0;
        o.numChannels_ = 0;
        o.sampleRate_  = 0.0;
    }

    // ---- WORKER THREAD API -----------------------------------------------

    /// Load `path` into memory.  Supports IEEE float-32 and PCM int-16 WAV.
    /// Sets isLoaded() to true on success, leaves it false on any error.
    /// Calling load() a second time resets and re-reads.
    bool load(const std::filesystem::path& path)
    {
        isLoaded_.store(false, std::memory_order_release);
        samples_.clear();
        numFrames_   = 0;
        numChannels_ = 0;
        sampleRate_  = 0.0;

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
            return false;

        // ---- Parse RIFF/WAVE header ----
        char riff[4];
        if (!f.read(riff, 4)) return false;
        if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F')
            return false;

        f.ignore(4);   // file size

        char wave[4];
        if (!f.read(wave, 4)) return false;
        if (wave[0] != 'W' || wave[1] != 'A' || wave[2] != 'V' || wave[3] != 'E')
            return false;

        // ---- Scan chunks ----
        uint16_t audioFormat   = 0;
        uint16_t chans         = 0;
        uint32_t sr            = 0;
        uint16_t bitsPerSample = 0;
        uint32_t dataSize      = 0;
        bool     foundFmt      = false;
        bool     foundData     = false;

        while (f.good())
        {
            char id[4];
            if (!f.read(id, 4)) break;

            uint32_t chunkSize = readLE32(f);

            if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ')
            {
                audioFormat   = readLE16(f);
                chans         = readLE16(f);
                sr            = readLE32(f);
                f.ignore(4);  // byteRate
                f.ignore(2);  // blockAlign
                bitsPerSample = readLE16(f);
                // Skip any extra fmt bytes
                if (chunkSize > 16)
                    f.ignore(static_cast<std::streamsize>(chunkSize) - 16);
                foundFmt = true;
            }
            else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a')
            {
                dataSize  = chunkSize;
                foundData = true;
                break;  // data chunk follows immediately
            }
            else
            {
                f.ignore(static_cast<std::streamsize>(chunkSize));
            }
        }

        if (!foundFmt || !foundData || chans == 0 || sr == 0)
            return false;

        // 1 = PCM int, 3 = IEEE float
        if (audioFormat != 1 && audioFormat != 3)
            return false;

        numChannels_ = static_cast<int>(chans);
        sampleRate_  = static_cast<double>(sr);

        if (audioFormat == 3 && bitsPerSample == 32)
        {
            // IEEE float — read directly
            const uint32_t numSamples = dataSize / 4u;
            numFrames_ = static_cast<int64_t>(numSamples) / numChannels_;
            samples_.resize(numSamples);

            f.read(reinterpret_cast<char*>(samples_.data()),
                   static_cast<std::streamsize>(numSamples) * 4);
            if (!f) { samples_.clear(); return false; }
        }
        else if (audioFormat == 1 && bitsPerSample == 16)
        {
            // PCM int16 — convert to float
            const uint32_t numSamples = dataSize / 2u;
            numFrames_ = static_cast<int64_t>(numSamples) / numChannels_;
            samples_.resize(numSamples);

            std::vector<int16_t> raw(numSamples);
            f.read(reinterpret_cast<char*>(raw.data()),
                   static_cast<std::streamsize>(numSamples) * 2);
            if (!f) { samples_.clear(); return false; }

            constexpr float kScale = 1.0f / 32768.0f;
            for (uint32_t i = 0; i < numSamples; ++i)
                samples_[i] = static_cast<float>(raw[i]) * kScale;
        }
        else
        {
            return false;  // unsupported bit depth
        }

        isLoaded_.store(true, std::memory_order_release);
        return true;
    }

    /// Remove all loaded data and reset to the unloaded state.
    void reset() noexcept
    {
        isLoaded_.store(false, std::memory_order_release);
        samples_.clear();
        numFrames_   = 0;
        numChannels_ = 0;
        sampleRate_  = 0.0;
    }

    // ---- ANY-THREAD queries (safe after isLoaded() == true) --------------

    [[nodiscard]] bool    isLoaded()    const noexcept { return isLoaded_.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t numFrames()   const noexcept { return numFrames_; }
    [[nodiscard]] int     numChannels() const noexcept { return numChannels_; }
    [[nodiscard]] double  sampleRate()  const noexcept { return sampleRate_; }

    // ---- AUDIO THREAD read -----------------------------------------------

    /// Read `numFrames` interleaved frames starting at `startFrame` into `out`.
    /// `out` must point to at least numFrames * numChannels() floats.
    /// Frames outside [0, numFrames()) are filled with silence.
    /// AUDIO THREAD SAFE: no allocation, no locking, no I/O.
    void read(int64_t startFrame, int numFrames, float* out, int outChannels) const noexcept
    {
        if (!isLoaded_.load(std::memory_order_acquire) || out == nullptr || numFrames <= 0)
        {
            if (out && numFrames > 0)
                std::memset(out, 0, static_cast<std::size_t>(numFrames) *
                                    static_cast<std::size_t>(outChannels) * sizeof(float));
            return;
        }

        const int   srcCh = numChannels_;
        const auto  total = static_cast<int64_t>(samples_.size());

        for (int i = 0; i < numFrames; ++i)
        {
            const int64_t srcFrame = startFrame + i;

            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int64_t idx = srcFrame * srcCh + (ch % srcCh);
                out[i * outChannels + ch] =
                    (srcFrame >= 0 && idx < total) ? samples_[static_cast<std::size_t>(idx)] : 0.0f;
            }
        }
    }

private:
    static uint16_t readLE16(std::ifstream& f)
    {
        uint8_t b[2] = {};
        f.read(reinterpret_cast<char*>(b), 2);
        return static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8u);
    }

    static uint32_t readLE32(std::ifstream& f)
    {
        uint8_t b[4] = {};
        f.read(reinterpret_cast<char*>(b), 4);
        return static_cast<uint32_t>(b[0])
             | (static_cast<uint32_t>(b[1]) << 8u)
             | (static_cast<uint32_t>(b[2]) << 16u)
             | (static_cast<uint32_t>(b[3]) << 24u);
    }

    std::vector<float> samples_;
    int64_t            numFrames_   { 0 };
    int                numChannels_ { 0 };
    double             sampleRate_  { 0.0 };
    std::atomic<bool>  isLoaded_    { false };
};

} // namespace ssbb
