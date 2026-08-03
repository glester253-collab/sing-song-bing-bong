#pragma once
// PlaybackBuffer.h - worker-loaded, real-time-safe playback of the latest take.
//
// The worker thread loads a completed float-32 WAV into one of three slots and
// publishes it with a release store. The audio thread only reads immutable
// sample memory. Per-slot reader counters prevent a later worker load from
// reusing a slot while an audio callback is still reading it.

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace ssbb {

class PlaybackBuffer
{
public:
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static_assert(std::atomic<int64_t>::is_always_lock_free);

    PlaybackBuffer() = default;
    PlaybackBuffer(const PlaybackBuffer&) = delete;
    PlaybackBuffer& operator=(const PlaybackBuffer&) = delete;

    // WORKER THREAD ONLY. Supports the float-32 mono/stereo files produced by
    // WavWriter. The currently published take remains available on failure.
    bool load(const std::filesystem::path& path)
    {
        LoadedWav loaded;
        if (!readWav(path, loaded))
            return false;

        const int active = activeSlot_.load(std::memory_order_acquire);
        int target = -1;
        for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
        {
            if (i != active && slots_[static_cast<std::size_t>(i)].readers.load(
                                   std::memory_order_acquire) == 0)
            {
                target = i;
                break;
            }
        }
        if (target < 0)
            return false;

        auto& slot = slots_[static_cast<std::size_t>(target)];
        slot.samples = std::move(loaded.samples);
        slot.frames = loaded.frames;
        slot.channels = loaded.channels;
        slot.sampleRate = loaded.sampleRate;
        publishedFrames_.store(loaded.frames, std::memory_order_release);
        activeSlot_.store(target, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool isReady() const noexcept
    {
        return activeSlot_.load(std::memory_order_acquire) >= 0;
    }

    [[nodiscard]] int64_t numFrames() const noexcept
    {
        return publishedFrames_.load(std::memory_order_acquire);
    }

    // AUDIO THREAD ONLY. Adds the take to planar output buffers at the given
    // timeline position. No allocation, locks, file access, or exceptions.
    void render(float* const* outputChannelData,
                int numOutputChannels,
                int numSamples,
                int64_t blockStartSamples,
                double deviceSampleRate) noexcept
    {
        if (outputChannelData == nullptr || numOutputChannels <= 0 ||
            numSamples <= 0 || blockStartSamples < 0)
            return;

        const int active = activeSlot_.load(std::memory_order_acquire);
        if (active < 0)
            return;

        auto& slot = slots_[static_cast<std::size_t>(active)];
        slot.readers.fetch_add(1, std::memory_order_acq_rel);

        // Recorded takes are played only at their native sample rate. A device
        // rate change fails silent until a resampler is implemented.
        if (std::abs(slot.sampleRate - deviceSampleRate) <= 0.5 &&
            blockStartSamples < slot.frames)
        {
            const int64_t available = slot.frames - blockStartSamples;
            const int framesToRender = static_cast<int>(
                available < numSamples ? available : numSamples);

            for (int ch = 0; ch < numOutputChannels; ++ch)
            {
                float* out = outputChannelData[ch];
                if (out == nullptr)
                    continue;

                const int sourceChannel = ch % slot.channels;
                for (int i = 0; i < framesToRender; ++i)
                {
                    const auto frame = blockStartSamples + i;
                    const auto index = static_cast<std::size_t>(
                        frame * slot.channels + sourceChannel);
                    const float sample = slot.samples[index];
                    if (std::isfinite(sample))
                        out[i] += sample;
                }
            }
        }

        slot.readers.fetch_sub(1, std::memory_order_release);
    }

private:
    struct Slot
    {
        std::vector<float> samples;
        int64_t frames { 0 };
        int channels { 0 };
        double sampleRate { 0.0 };
        std::atomic<unsigned> readers { 0 };
    };

    struct LoadedWav
    {
        std::vector<float> samples;
        int64_t frames { 0 };
        int channels { 0 };
        double sampleRate { 0.0 };
    };

    static uint16_t readLE16(std::istream& stream)
    {
        unsigned char bytes[2] {};
        stream.read(reinterpret_cast<char*>(bytes), 2);
        return static_cast<uint16_t>(bytes[0]) |
               (static_cast<uint16_t>(bytes[1]) << 8u);
    }

    static uint32_t readLE32(std::istream& stream)
    {
        unsigned char bytes[4] {};
        stream.read(reinterpret_cast<char*>(bytes), 4);
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8u) |
               (static_cast<uint32_t>(bytes[2]) << 16u) |
               (static_cast<uint32_t>(bytes[3]) << 24u);
    }

    static bool readWav(const std::filesystem::path& path, LoadedWav& out)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
            return false;

        char id[4] {};
        stream.read(id, 4);
        if (!stream || id[0] != 'R' || id[1] != 'I' || id[2] != 'F' || id[3] != 'F')
            return false;
        (void)readLE32(stream);
        stream.read(id, 4);
        if (!stream || id[0] != 'W' || id[1] != 'A' || id[2] != 'V' || id[3] != 'E')
            return false;

        uint16_t format = 0;
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t bitsPerSample = 0;
        uint32_t dataBytes = 0;
        std::streampos dataPosition {};
        bool foundFormat = false;

        while (stream)
        {
            stream.read(id, 4);
            if (!stream)
                break;
            const uint32_t chunkBytes = readLE32(stream);
            if (!stream)
                return false;

            if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ')
            {
                if (chunkBytes < 16)
                    return false;
                format = readLE16(stream);
                channels = readLE16(stream);
                sampleRate = readLE32(stream);
                (void)readLE32(stream);
                (void)readLE16(stream);
                bitsPerSample = readLE16(stream);
                if (chunkBytes > 16)
                    stream.ignore(static_cast<std::streamsize>(chunkBytes - 16));
                foundFormat = static_cast<bool>(stream);
            }
            else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a')
            {
                dataBytes = chunkBytes;
                dataPosition = stream.tellg();
                break;
            }
            else
            {
                stream.ignore(static_cast<std::streamsize>(chunkBytes));
            }

            if ((chunkBytes & 1u) != 0u)
                stream.ignore(1);
        }

        if (!foundFormat || dataBytes == 0 || dataPosition == std::streampos(-1) ||
            format != 3 || bitsPerSample != 32 || channels == 0 ||
            channels > 2 || sampleRate == 0)
            return false;

        const uint64_t sampleCount64 = static_cast<uint64_t>(dataBytes) / sizeof(float);
        if (dataBytes % sizeof(float) != 0 || sampleCount64 % channels != 0 ||
            sampleCount64 > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
            return false;

        out.samples.resize(static_cast<std::size_t>(sampleCount64));
        stream.seekg(dataPosition);
        stream.read(reinterpret_cast<char*>(out.samples.data()),
                    static_cast<std::streamsize>(dataBytes));
        if (!stream)
            return false;

        out.frames = static_cast<int64_t>(sampleCount64 / channels);
        out.channels = static_cast<int>(channels);
        out.sampleRate = static_cast<double>(sampleRate);
        return out.frames > 0;
    }

    std::array<Slot, 3> slots_;
    std::atomic<int> activeSlot_ { -1 };
    std::atomic<int64_t> publishedFrames_ { 0 };
};

} // namespace ssbb
