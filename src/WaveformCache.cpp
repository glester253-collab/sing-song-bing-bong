// WaveformCache.cpp
// WORKER THREAD — file I/O and DSP analysis only; no audio-thread work here.
#include "WaveformCache.h"

#include <cmath>      // std::sqrt, std::isfinite
#include <cstring>    // std::memcmp
#include <fstream>

namespace ssbb {

namespace {

// ---- Portable little-endian readers ------------------------------------
// These work on both little-endian (x86/x64) and big-endian hosts.

uint16_t readLE16(std::ifstream& f) noexcept
{
    uint8_t b[2] {};
    f.read(reinterpret_cast<char*>(b), 2);
    return static_cast<uint16_t>(b[0])
         | (static_cast<uint16_t>(b[1]) << 8u);
}

uint32_t readLE32(std::ifstream& f) noexcept
{
    uint8_t b[4] {};
    f.read(reinterpret_cast<char*>(b), 4);
    return static_cast<uint32_t>(b[0])
         | (static_cast<uint32_t>(b[1]) << 8u)
         | (static_cast<uint32_t>(b[2]) << 16u)
         | (static_cast<uint32_t>(b[3]) << 24u);
}

// Convert a raw int16 sample to [-1.0, 1.0].
float int16ToFloat(int16_t v) noexcept
{
    return static_cast<float>(v) / 32768.0f;
}

} // namespace

// ---- WaveformCache::buildFromFile --------------------------------------

void WaveformCache::buildFromFile(const std::filesystem::path& wavPath,
                                  int samplesPerPixel)
{
    // Always start from a clean state.
    isReady_.store(false, std::memory_order_release);
    frames_.clear();

    if (samplesPerPixel <= 0)
        return;

    std::ifstream f(wavPath, std::ios::binary);
    if (!f.is_open())
        return;

    // ---- RIFF/WAVE header --------------------------------------------------
    char riff[4] {};
    f.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) return;

    /*riffSize =*/ readLE32(f);   // skip: we use the data chunk size instead

    char wave[4] {};
    f.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) return;

    // ---- Chunk scan --------------------------------------------------------
    // We only care about "fmt " and "data"; all others are skipped.
    uint16_t audioFormat   = 0;
    uint16_t numChannels   = 0;
    uint16_t bitsPerSample = 0;
    bool     foundFmt      = false;

    while (f && f.good())
    {
        char chunkId[4] {};
        f.read(chunkId, 4);
        if (f.gcount() < 4) break;

        const uint32_t chunkSize = readLE32(f);
        const auto chunkDataStart = f.tellg();

        if (std::memcmp(chunkId, "fmt ", 4) == 0)
        {
            audioFormat   = readLE16(f);          // 1 = PCM, 3 = IEEE float
            numChannels   = readLE16(f);
            /*sampleRate*/   readLE32(f);          // not needed for cache
            /*byteRate*/     readLE32(f);
            /*blockAlign*/   readLE16(f);
            bitsPerSample = readLE16(f);
            foundFmt = true;

            // Skip any extra fmt bytes (e.g. extensible format).
            if (chunkSize > 16u)
            {
                f.seekg(chunkDataStart);
                f.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
            }
        }
        else if (std::memcmp(chunkId, "data", 4) == 0 && foundFmt)
        {
            // Only float-32 and int-16 are supported.
            const bool isFloat = (audioFormat == 3 && bitsPerSample == 32);
            const bool isPCM16 = (audioFormat == 1 && bitsPerSample == 16);

            if (!isFloat && !isPCM16)
                return;

            if (numChannels == 0)
                return;

            const int totalFrames =
                (numChannels * (bitsPerSample / 8) > 0)
                ? static_cast<int>(
                      chunkSize / static_cast<uint32_t>(numChannels)
                               / static_cast<uint32_t>(bitsPerSample / 8))
                : 0;

            // Process samples directly from the file stream, frame by frame.
            // kReadBatch caps how many source frames we count per outer iteration
            // to avoid spending too long inside any single lock window.
            constexpr int kReadBatch = 2048;

            int   srcFramesLeft = totalFrames;
            int   accFrames     = 0;
            float peakPos       = 0.0f;
            float peakNeg       = 0.0f;
            double sumSq        = 0.0;

            while (srcFramesLeft > 0 && f.good())
            {
                const int toRead = (srcFramesLeft < kReadBatch)
                                   ? srcFramesLeft : kReadBatch;

                for (int i = 0; i < toRead; ++i)
                {
                    // Average channels into a mono sample.
                    float chSum = 0.0f;
                    for (int ch = 0; ch < numChannels; ++ch)
                    {
                        if (isFloat)
                        {
                            float s = 0.0f;
                            f.read(reinterpret_cast<char*>(&s), 4);
                            chSum += s;
                        }
                        else
                        {
                            int16_t s = 0;
                            f.read(reinterpret_cast<char*>(&s), 2);
                            chSum += int16ToFloat(s);
                        }
                    }

                    float mono = chSum / static_cast<float>(numChannels);

                    // Reject non-finite values (NaN / Inf / denormals guard).
                    if (!std::isfinite(mono)) mono = 0.0f;

                    if (mono > peakPos)  peakPos = mono;
                    if (mono < peakNeg)  peakNeg = mono;
                    sumSq += static_cast<double>(mono) * static_cast<double>(mono);
                    ++accFrames;

                    if (accFrames >= samplesPerPixel)
                    {
                        Frame fr;
                        fr.peakPos = peakPos;
                        fr.peakNeg = peakNeg;
                        fr.rms     = static_cast<float>(
                            std::sqrt(sumSq / static_cast<double>(accFrames)));
                        frames_.push_back(fr);

                        peakPos   = 0.0f;
                        peakNeg   = 0.0f;
                        sumSq     = 0.0;
                        accFrames = 0;
                    }
                }

                srcFramesLeft -= toRead;
            }

            // Flush any partial final frame.
            if (accFrames > 0)
            {
                Frame fr;
                fr.peakPos = peakPos;
                fr.peakNeg = peakNeg;
                fr.rms     = static_cast<float>(
                    std::sqrt(sumSq / static_cast<double>(accFrames)));
                frames_.push_back(fr);
            }

            // Signal that the cache is ready.  Use release ordering so that
            // any thread that observes isReady_ == true also sees the frame data.
            isReady_.store(true, std::memory_order_release);
            return;
        }
        else
        {
            // Unknown chunk — skip it.
            f.seekg(chunkDataStart);
            f.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
        }
    }
    // If we reach here without entering the data branch, isReady_ stays false.
}

} // namespace ssbb
