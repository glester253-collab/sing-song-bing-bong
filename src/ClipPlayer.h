#pragma once
// ClipPlayer.h — audio-thread-safe single-clip playback engine.
//
// DESIGN
// ------
// Loading audio happens on the WORKER/MESSAGE thread (ClipPlayer::load).
// The audio thread only reads a pre-loaded buffer via raw pointer + length
// atomics — no allocation, no locks, no file I/O on the audio path.
//
// BUFFER-SWAP SAFETY
// ------------------
// The message thread must call stop() and wait for the audio callback to
// finish its current block before calling load() with a new buffer.  In
// practice, calling stop() then load() from the message thread (which is
// not the audio thread) is safe because the next audio callback will see
// `playing_` == false before it touches the buffer pointer.
//
// USAGE
//   // Worker/message thread:
//   ClipPlayer player;
//   player.prepare(sampleRate, blockSize);
//   player.load(samplesVector, numChannels);
//
//   // Message thread:
//   player.play();
//   player.stop();
//   player.seekToStart();
//
//   // Audio thread:
//   player.processBlock(outputData, numOutputCh, numSamples);

#include <atomic>
#include <cstdint>
#include <vector>

namespace ssbb {

class ClipPlayer
{
public:
    ClipPlayer()  = default;

    // Non-copyable / non-movable (contains atomics).
    ClipPlayer(const ClipPlayer&)            = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;
    ClipPlayer(ClipPlayer&&)                 = delete;
    ClipPlayer& operator=(ClipPlayer&&)      = delete;

    // ---- Preparation (device setup thread or message thread) ------------

    /// Cache sample rate and block size — currently informational.
    void prepare(double /*sampleRate*/, int /*blockSize*/) noexcept {}

    // ---- Loading (WORKER / MESSAGE thread — NOT audio thread) -----------

    /// Replace the playback buffer.
    /// @param samples      Interleaved float samples (all channels).
    /// @param numChannels  Number of channels in the interleaved data.
    ///
    /// MUST be called only when playing_ == false (i.e. after stop()).
    /// The caller guarantees the audio thread is not accessing the old buffer
    /// (stop() sets playing_ = false; the audio thread checks playing_ first).
    void load(std::vector<float> samples, int numChannels) noexcept
    {
        // Replace the buffer while audio thread is not reading it.
        samples_     = std::move(samples);
        numChannels_ = numChannels > 0 ? numChannels : 1;

        // Publish new pointer + length atomically (audio thread not running).
        activePtr_.store(samples_.data(), std::memory_order_release);
        activeLen_.store(static_cast<int64_t>(samples_.size()),
                         std::memory_order_release);
        position_.store(0, std::memory_order_release);
    }

    // ---- Message-thread controls ----------------------------------------

    /// Start / resume playback from the current position.
    void play() noexcept
    {
        playing_.store(true, std::memory_order_release);
    }

    /// Stop playback.  The audio thread will observe this on its next callback.
    void stop() noexcept
    {
        playing_.store(false, std::memory_order_release);
    }

    /// Seek to the beginning of the clip (safe to call while stopped).
    void seekToStart() noexcept
    {
        position_.store(0, std::memory_order_release);
    }

    /// True if the clip is currently playing (or very nearly done).
    bool isPlaying() const noexcept
    {
        return playing_.load(std::memory_order_acquire);
    }

    /// Playback position as a sample-frame index.
    int64_t positionFrames() const noexcept
    {
        return position_.load(std::memory_order_relaxed);
    }

    /// Total frames in the loaded clip (0 if nothing is loaded).
    int64_t totalFrames() const noexcept
    {
        const int64_t len = activeLen_.load(std::memory_order_relaxed);
        const int     ch  = numChannels_;
        return (ch > 0) ? (len / ch) : 0;
    }

    // ---- AUDIO THREAD ---------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Mix the clip into the output buffers.
    /// If the clip runs out, playing_ is cleared atomically (no alloc/lock).
    void processBlock(float* const* outputChannelData,
                      int           numOutputChannels,
                      int           numSamples) noexcept
    {
        if (!playing_.load(std::memory_order_acquire)) return;

        const float* buf  = activePtr_.load(std::memory_order_acquire);
        const int64_t len = activeLen_.load(std::memory_order_acquire);
        if (buf == nullptr || len <= 0 || numOutputChannels <= 0) return;

        int64_t pos = position_.load(std::memory_order_relaxed);
        const int ch = numChannels_;

        for (int s = 0; s < numSamples; ++s)
        {
            // Frame index in the interleaved buffer.
            const int64_t frameStart = pos * static_cast<int64_t>(ch);
            if (frameStart >= len)
            {
                // Clip finished — stop.
                playing_.store(false, std::memory_order_release);
                break;
            }

            // Mix each source channel to the matching output channel (or ch 0).
            for (int outCh = 0; outCh < numOutputChannels; ++outCh)
            {
                if (outputChannelData[outCh] == nullptr) continue;
                const int srcCh = (ch == 1) ? 0 : (outCh < ch ? outCh : ch - 1);
                const int64_t idx = frameStart + static_cast<int64_t>(srcCh);
                outputChannelData[outCh][s] += buf[idx];
            }

            ++pos;
        }

        position_.store(pos, std::memory_order_release);

        // If we consumed the last frame in this block, stop now.
        if (pos * static_cast<int64_t>(ch) >= len)
            playing_.store(false, std::memory_order_release);
    }

private:
    // Loaded buffer (worker/message thread writes, audio thread reads).
    std::vector<float> samples_;
    int                numChannels_ { 1 };

    // Atomics so the audio thread can safely read without a lock.
    // Updated only when playing_ == false.
    std::atomic<const float*> activePtr_ { nullptr };
    std::atomic<int64_t>      activeLen_ { 0 };

    // Position is read/written by the audio thread; read by message thread.
    std::atomic<int64_t>      position_  { 0 };
    std::atomic<bool>         playing_   { false };
};

} // namespace ssbb
