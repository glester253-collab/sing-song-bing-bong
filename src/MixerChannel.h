#pragma once
// MixerChannel.h — single channel strip: gain, pan, mute, solo, pre/post send,
//                  and peak metering.
//
// THREAD MODEL:
//   Message thread : all setters.
//   Audio thread   : processBlock() reads atomics, mixes into the bus buffers,
//                    and updates peak meters.
//                    NO allocation, NO locks, NO I/O, NO logging.
//
// Bus routing:
//   The channel has one pre-fader send and one post-fader send.
//   Levels are independent atomics so the message thread can update them safely.

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace ssbb {

class MixerChannel
{
public:
    static constexpr int kMaxOutputChannels = 2;

    MixerChannel() = default;

    // Non-copyable (atomics).
    MixerChannel(const MixerChannel&)            = delete;
    MixerChannel& operator=(const MixerChannel&) = delete;
    MixerChannel(MixerChannel&&)                 = delete;
    MixerChannel& operator=(MixerChannel&&)      = delete;

    // ---- Message-thread API ----------------------------------------------

    /// Fader gain (linear, 0.0–2.0).  0 dBFS = 1.0.
    void setGain(float g) noexcept
    {
        gain_.store(g >= 0.0f ? g : 0.0f, std::memory_order_relaxed);
    }

    float getGain() const noexcept { return gain_.load(std::memory_order_relaxed); }

    /// Pan position: -1.0 (full left) to +1.0 (full right).
    void setPan(float pan) noexcept
    {
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;
        pan_.store(pan, std::memory_order_relaxed);
    }

    float getPan() const noexcept { return pan_.load(std::memory_order_relaxed); }

    void setMute(bool mute) noexcept { mute_.store(mute, std::memory_order_relaxed); }
    bool isMuted()  const noexcept   { return mute_.load(std::memory_order_relaxed); }

    void setSolo(bool solo) noexcept { solo_.store(solo, std::memory_order_relaxed); }
    bool isSoloed() const noexcept   { return solo_.load(std::memory_order_relaxed); }

    /// Pre-fader send level (linear).
    void setPreSendLevel(float lvl) noexcept
    {
        preSend_.store(lvl >= 0.0f ? lvl : 0.0f, std::memory_order_relaxed);
    }

    /// Post-fader send level (linear).
    void setPostSendLevel(float lvl) noexcept
    {
        postSend_.store(lvl >= 0.0f ? lvl : 0.0f, std::memory_order_relaxed);
    }

    // ---- Peak meter (any-thread read after processBlock) -----------------

    /// Peak value (linear) of the last processed block, per output channel.
    float getPeakLevel(int ch) const noexcept
    {
        if (ch < 0 || ch >= kMaxOutputChannels) return 0.0f;
        return peakLevel_[static_cast<std::size_t>(ch)].load(std::memory_order_relaxed);
    }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Mix the mono input `in[numSamples]` into the stereo main bus and,
    /// optionally, into pre/post send buses.
    ///
    /// @param in               Mono input buffer.
    /// @param mainBusL         Main bus left output (additive mix-in).
    /// @param mainBusR         Main bus right output (additive mix-in).
    /// @param preBusL          Pre-fader send bus left (additive, may be nullptr).
    /// @param preBusR          Pre-fader send bus right (additive, may be nullptr).
    /// @param postBusL         Post-fader send bus left (additive, may be nullptr).
    /// @param postBusR         Post-fader send bus right (additive, may be nullptr).
    /// @param numSamples       Block size.
    /// @param anySoloed        True if any channel in the mixer is soloed.
    void processBlock(const float* in,
                      float*       mainBusL,
                      float*       mainBusR,
                      float*       preBusL,
                      float*       preBusR,
                      float*       postBusL,
                      float*       postBusR,
                      int          numSamples,
                      bool         anySoloed) noexcept
    {
        if (numSamples <= 0 || in == nullptr) return;

        const bool muted = mute_.load(std::memory_order_relaxed);
        const bool solod = solo_.load(std::memory_order_relaxed);

        // In solo mode: silence channels that are not soloed.
        if (anySoloed && !solod) return;
        if (muted) return;

        const float gain    = gain_.load(std::memory_order_relaxed);
        const float pan     = pan_ .load(std::memory_order_relaxed);
        const float preL    = preSend_.load(std::memory_order_relaxed);
        const float postL   = postSend_.load(std::memory_order_relaxed);

        // Constant-power pan: left = cos(θ), right = sin(θ), θ in [0, π/2].
        const float angle = (pan + 1.0f) * 0.25f * 3.14159265f; // [0, π/2]
        const float panL  = cosFast(angle);
        const float panR  = sinFast(angle);

        float peakL = 0.0f, peakR = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float s = in[i];

            // Pre-fader send (no gain/pan applied).
            if (preBusL) preBusL[i] += s * preL;
            if (preBusR) preBusR[i] += s * preL;

            // Apply fader gain.
            const float faded = s * gain;

            // Post-fader send.
            if (postBusL) postBusL[i] += faded * postL;
            if (postBusR) postBusR[i] += faded * postL;

            // Apply pan and mix into main bus.
            const float l = faded * panL;
            const float r = faded * panR;
            if (mainBusL) mainBusL[i] += l;
            if (mainBusR) mainBusR[i] += r;

            // Peak detect.
            const float absL = l < 0.0f ? -l : l;
            const float absR = r < 0.0f ? -r : r;
            if (absL > peakL) peakL = absL;
            if (absR > peakR) peakR = absR;
        }

        peakLevel_[0].store(peakL, std::memory_order_relaxed);
        peakLevel_[1].store(peakR, std::memory_order_relaxed);
    }

private:
    std::atomic<float> gain_     { 1.0f };
    std::atomic<float> pan_      { 0.0f };
    std::atomic<bool>  mute_     { false };
    std::atomic<bool>  solo_     { false };
    std::atomic<float> preSend_  { 0.0f };
    std::atomic<float> postSend_ { 0.0f };

    std::array<std::atomic<float>, kMaxOutputChannels> peakLevel_ {};

    // Fast trig approximations — no libm call on the audio thread.
    static float cosFast(float x) noexcept
    {
        // Taylor around 0: cos(x) ≈ 1 - x²/2 + x⁴/24  (good for [0, π/2])
        const float x2 = x * x;
        return 1.0f - x2 * (0.5f - x2 * 0.041667f);
    }
    static float sinFast(float x) noexcept
    {
        // Taylor: sin(x) ≈ x - x³/6 + x⁵/120  (good for [0, π/2])
        const float x2 = x * x;
        return x * (1.0f - x2 * (0.16667f - x2 * 0.00833f));
    }
};

} // namespace ssbb
