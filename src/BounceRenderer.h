#pragma once
// BounceRenderer.h — offline audio render for track freeze and session export.
//
// THREAD MODEL:
//   Worker thread : render() — all work happens here.  The audio device must
//                  be stopped or the caller must swap in a null callback before
//                  calling render() to avoid double-processing.
//   Message thread: start/stop control.
//
// The renderer processes audio in blocks by driving a user-supplied
// RenderCallback that mimics the real-time audio callback signature.
// The resulting interleaved float audio is written to a WAV file via WavWriter.

#include "WavWriter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace ssbb {

class BounceRenderer
{
public:
    /// Callback signature: fill `output` (numCh × numSamples interleaved floats).
    /// The callback must be audio-thread-safe (no allocation, no locks, no I/O).
    using RenderCallback = std::function<void(float** output,
                                              int     numChannels,
                                              int     numSamples,
                                              int64_t blockStartSample)>;

    BounceRenderer() = default;

    // Non-copyable / non-movable.
    BounceRenderer(const BounceRenderer&)            = delete;
    BounceRenderer& operator=(const BounceRenderer&) = delete;
    BounceRenderer(BounceRenderer&&)                 = delete;
    BounceRenderer& operator=(BounceRenderer&&)      = delete;

    // ---- Worker-thread API -----------------------------------------------

    /// Render `durationSamples` of audio starting at timeline sample 0
    /// by repeatedly calling `callback` with blocks of `blockSize` samples.
    ///
    /// @param outputPath       Destination WAV file path.
    /// @param callback         Renders one block of audio on each call.
    /// @param sampleRate       Session sample rate.
    /// @param numChannels      Number of output channels (1 or 2).
    /// @param durationSamples  Total samples to render.
    /// @param blockSize        Processing block size (default 512).
    /// @param cancelFlag       Set to true by the message thread to abort.
    /// @return true on success, false on I/O error or cancellation.
    bool render(const std::filesystem::path& outputPath,
                RenderCallback               callback,
                double                       sampleRate,
                int                          numChannels,
                int64_t                      durationSamples,
                int                          blockSize   = 512,
                const std::atomic<bool>*     cancelFlag  = nullptr)
    {
        if (!callback || numChannels < 1 || numChannels > 2 ||
            durationSamples <= 0 || blockSize <= 0)
            return false;

        WavWriter writer;
        if (!writer.open(outputPath, sampleRate, numChannels))
            return false;

        // Allocate per-channel and interleaved buffers on the worker stack/heap.
        // These exist only while render() runs — not on the audio thread.
        const std::size_t bsz = static_cast<std::size_t>(blockSize);
        const std::size_t chs = static_cast<std::size_t>(numChannels);

        std::vector<float>         interleaved(bsz * chs, 0.0f);
        std::vector<std::vector<float>> chBufs(chs, std::vector<float>(bsz, 0.0f));
        std::vector<float*>            chPtrs(chs);
        for (std::size_t c = 0; c < chs; ++c)
            chPtrs[c] = chBufs[c].data();

        int64_t pos = 0;
        while (pos < durationSamples)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            {
                writer.close();
                return false;   // cancelled
            }

            const int64_t remaining = durationSamples - pos;
            const int     frames    = static_cast<int>(
                (remaining < static_cast<int64_t>(blockSize))
                    ? remaining : static_cast<int64_t>(blockSize));

            // Clear channel buffers.
            for (auto& ch : chBufs)
                std::fill(ch.begin(), ch.begin() + frames, 0.0f);

            callback(chPtrs.data(), numChannels, frames, pos);

            // Interleave channels.
            for (int f = 0; f < frames; ++f)
                for (int c = 0; c < numChannels; ++c)
                    interleaved[static_cast<std::size_t>(f * numChannels + c)] =
                        chBufs[static_cast<std::size_t>(c)][static_cast<std::size_t>(f)];

            writer.write(interleaved.data(), frames * numChannels);
            pos += frames;
        }

        writer.close();
        return true;
    }

    /// Convenience: render to a path and report progress 0.0–1.0 via callback.
    bool renderWithProgress(const std::filesystem::path& outputPath,
                            RenderCallback               callback,
                            double                       sampleRate,
                            int                          numChannels,
                            int64_t                      durationSamples,
                            std::function<void(float)>   progressCb,
                            int                          blockSize  = 512,
                            const std::atomic<bool>*     cancelFlag = nullptr)
    {
        if (!callback || numChannels < 1 || numChannels > 2 ||
            durationSamples <= 0 || blockSize <= 0)
            return false;

        WavWriter writer;
        if (!writer.open(outputPath, sampleRate, numChannels))
            return false;

        const std::size_t bsz = static_cast<std::size_t>(blockSize);
        const std::size_t chs = static_cast<std::size_t>(numChannels);

        std::vector<float>              interleaved(bsz * chs, 0.0f);
        std::vector<std::vector<float>> chBufs(chs, std::vector<float>(bsz, 0.0f));
        std::vector<float*>             chPtrs(chs);
        for (std::size_t c = 0; c < chs; ++c)
            chPtrs[c] = chBufs[c].data();

        int64_t pos = 0;
        while (pos < durationSamples)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            {
                writer.close();
                return false;
            }

            const int64_t remaining = durationSamples - pos;
            const int     frames    = static_cast<int>(
                (remaining < static_cast<int64_t>(blockSize))
                    ? remaining : static_cast<int64_t>(blockSize));

            for (auto& ch : chBufs)
                std::fill(ch.begin(), ch.begin() + frames, 0.0f);

            callback(chPtrs.data(), numChannels, frames, pos);

            for (int f = 0; f < frames; ++f)
                for (int c = 0; c < numChannels; ++c)
                    interleaved[static_cast<std::size_t>(f * numChannels + c)] =
                        chBufs[static_cast<std::size_t>(c)][static_cast<std::size_t>(f)];

            writer.write(interleaved.data(), frames * numChannels);
            pos += frames;

            if (progressCb)
                progressCb(static_cast<float>(pos) /
                           static_cast<float>(durationSamples));
        }

        writer.close();
        return true;
    }
};

} // namespace ssbb
