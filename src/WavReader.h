#pragma once
// WavReader.h — header-only minimal IEEE 754 float-32 WAV reader.
//
// THREAD SAFETY: all methods must be called from the WORKER or MESSAGE thread
// only.  Never call WavReader methods from the audio thread.
//
// FORMAT: reads RIFF / WAVE files with a fmt chunk that uses either
//   AudioFormat = 3 (IEEE_FLOAT, 32-bit)  — the format written by WavWriter
//   AudioFormat = 1 (PCM, 16-bit)         — common import format
// Other formats / bit depths are rejected (open() returns false).
//
// The entire audio content is decoded into a std::vector<float> for
// audio-thread-safe playback via ClipPlayer.
//
// USAGE:
//   WavReader r;
//   if (r.open(path)) {
//       const auto& samples = r.samples();  // interleaved float frames
//       int ch  = r.numChannels();
//       double sr = r.sampleRate();
//   }

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ssbb {

class WavReader
{
public:
    WavReader()  = default;

    // Non-copyable
    WavReader(const WavReader&)            = delete;
    WavReader& operator=(const WavReader&) = delete;

    /// Open and decode a WAV file.  Returns false on any parse or I/O error.
    /// Must NOT be called from the audio thread.
    bool open(const std::filesystem::path& path) noexcept
    {
        samples_.clear();
        numChannels_  = 0;
        sampleRate_   = 0.0;

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        // ---- RIFF header ----
        char riff[4] {};
        f.read(riff, 4);
        if (!f || std::memcmp(riff, "RIFF", 4) != 0) return false;

        readLE32(f);            // file size minus 8 (ignore)

        char wave[4] {};
        f.read(wave, 4);
        if (!f || std::memcmp(wave, "WAVE", 4) != 0) return false;

        // ---- Scan chunks ----
        uint16_t audioFormat  = 0;
        uint16_t numCh        = 0;
        uint32_t sr           = 0;
        uint16_t bitsPerSample = 0;
        bool     hasFmt       = false;
        bool     hasData      = false;

        while (f.good() && !hasData)
        {
            char  id[4] {};
            f.read(id, 4);
            if (!f) break;

            const uint32_t chunkSize = readLE32(f);
            if (!f) break;

            if (std::memcmp(id, "fmt ", 4) == 0)
            {
                if (chunkSize < 16) return false;

                audioFormat   = readLE16(f);
                numCh         = readLE16(f);
                sr            = readLE32(f);
                /* byteRate */   readLE32(f);
                /* blockAlign */ readLE16(f);
                bitsPerSample = readLE16(f);

                // Skip any extra fmt bytes (e.g. extensible format header)
                if (chunkSize > 16)
                    f.seekg(static_cast<std::streamoff>(chunkSize - 16), std::ios::cur);

                if (audioFormat != 1 && audioFormat != 3) return false;
                if (numCh == 0) return false;
                if (audioFormat == 1 && bitsPerSample != 16) return false;
                if (audioFormat == 3 && bitsPerSample != 32) return false;

                hasFmt = true;
            }
            else if (std::memcmp(id, "data", 4) == 0)
            {
                if (!hasFmt) return false;

                if (audioFormat == 3)
                {
                    // IEEE float — direct read
                    const uint32_t numSamples =
                        chunkSize / static_cast<uint32_t>(sizeof(float));
                    samples_.resize(numSamples);
                    f.read(reinterpret_cast<char*>(samples_.data()),
                           static_cast<std::streamsize>(sizeof(float)) * numSamples);
                    if (!f) return false;
                }
                else
                {
                    // PCM 16-bit — convert to float
                    const uint32_t numSamples =
                        chunkSize / static_cast<uint32_t>(sizeof(int16_t));
                    std::vector<int16_t> raw(numSamples);
                    f.read(reinterpret_cast<char*>(raw.data()),
                           static_cast<std::streamsize>(sizeof(int16_t)) * numSamples);
                    if (!f) return false;

                    samples_.resize(numSamples);
                    constexpr float kScale = 1.0f / 32768.0f;
                    for (uint32_t i = 0; i < numSamples; ++i)
                        samples_[i] = static_cast<float>(raw[i]) * kScale;
                }

                numChannels_ = static_cast<int>(numCh);
                sampleRate_  = static_cast<double>(sr);
                hasData      = true;
            }
            else
            {
                // Unknown chunk — skip it
                // chunkSize is padded to even number per RIFF spec
                const uint32_t skip = chunkSize + (chunkSize & 1u);
                f.seekg(static_cast<std::streamoff>(skip), std::ios::cur);
            }
        }

        return hasData;
    }

    /// Decoded samples, interleaved across channels.
    const std::vector<float>& samples()     const noexcept { return samples_; }

    /// Number of channels (1 = mono, 2 = stereo, …)
    int    numChannels() const noexcept { return numChannels_; }

    /// Sample rate in Hz.
    double sampleRate()  const noexcept { return sampleRate_; }

    /// Total number of multichannel frames: samples().size() / numChannels().
    int64_t numFrames()  const noexcept
    {
        if (numChannels_ <= 0) return 0;
        return static_cast<int64_t>(samples_.size()) /
               static_cast<int64_t>(numChannels_);
    }

private:
    static uint16_t readLE16(std::ifstream& f) noexcept
    {
        uint8_t b[2] {};
        f.read(reinterpret_cast<char*>(b), 2);
        return static_cast<uint16_t>(b[0]) |
               (static_cast<uint16_t>(b[1]) << 8u);
    }

    static uint32_t readLE32(std::ifstream& f) noexcept
    {
        uint8_t b[4] {};
        f.read(reinterpret_cast<char*>(b), 4);
        return static_cast<uint32_t>(b[0])
             | (static_cast<uint32_t>(b[1]) <<  8u)
             | (static_cast<uint32_t>(b[2]) << 16u)
             | (static_cast<uint32_t>(b[3]) << 24u);
    }

    std::vector<float> samples_;
    int                numChannels_ { 0 };
    double             sampleRate_  { 0.0 };
};

} // namespace ssbb
