#pragma once
// PadEngine.h — real-time pad-hit processor and sample playback mixer.
//
// THREAD MODEL:
//   Message thread : triggerPad() queues a hit event.
//   Audio thread   : processBlock() drains the SPSC queue, fires samples,
//                    and mixes them into the output.  NO allocation, NO locks,
//                    NO file I/O, NO logging, NO exceptions.
//   Worker thread  : loadSample() loads WAV data for a pad slot.
//
// Design:
//   - PadHitQueue<kQueueSize> is a power-of-2 SPSC ring buffer of PadHitEvent.
//   - Each pad may have one pre-loaded WavReader.  Once loaded, the WavReader
//     buffer is immutable and safe for the audio thread to read.
//   - Each voice slot tracks the current playback position in the loaded buffer.
//     Up to kMaxVoices voices per pad can play simultaneously.

#include "DrumPad.h"
#include "WavReader.h"
#include "RecordBuffer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace ssbb {

class PadEngine
{
public:
    static constexpr int kNumPads    = DrumPad::kNumPads; // 16
    static constexpr int kMaxVoices  = 4;   // simultaneous voices per pad
    static constexpr int kQueueSize  = 256; // SPSC hit queue capacity (power of 2)

    PadEngine() = default;

    // Non-copyable / non-movable (atomics + arrays of atomics).
    PadEngine(const PadEngine&)            = delete;
    PadEngine& operator=(const PadEngine&) = delete;
    PadEngine(PadEngine&&)                 = delete;
    PadEngine& operator=(PadEngine&&)      = delete;

    // ---- Prepare (device setup thread) -----------------------------------

    void prepare(double sampleRate, int /*blockSize*/) noexcept
    {
        sampleRate_.store(sampleRate > 0.0 ? sampleRate : 44100.0,
                          std::memory_order_relaxed);
    }

    // ---- Message-thread API ----------------------------------------------

    /// Set the pad definition (name, note, sample path, default velocity).
    void setPad(int index, const DrumPad& pad)
    {
        if (index < 0 || index >= kNumPads) return;
        pads_[static_cast<std::size_t>(index)] = pad;
    }

    const DrumPad& getPad(int index) const noexcept
    {
        return pads_[static_cast<std::size_t>(
            (index >= 0 && index < kNumPads) ? index : 0)];
    }

    /// Trigger a pad hit from the UI / MIDI input.  Thread-safe.
    /// Returns false if the hit queue is full (event dropped).
    bool triggerPad(int padIndex, int velocity, int64_t sampleTime) noexcept
    {
        if (padIndex < 0 || padIndex >= kNumPads) return false;
        if (velocity < 0)   velocity = 0;
        if (velocity > 127) velocity = 127;

        PadHitEvent ev { padIndex, velocity, sampleTime };
        return hitQueue_.write(reinterpret_cast<const float*>(&ev),
                               static_cast<int>(sizeof(ev) / sizeof(float)));
    }

    // ---- Worker-thread API -----------------------------------------------

    /// Load the WAV file associated with pad `index`.
    /// After this returns, the audio thread may play the sample.
    bool loadSample(int index)
    {
        if (index < 0 || index >= kNumPads) return false;
        const auto& path = pads_[static_cast<std::size_t>(index)].samplePath;
        if (path.empty()) return false;
        return readers_[static_cast<std::size_t>(index)].load(path);
    }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Mix all active pad voices into outputChannelData.
    /// velocity scaling is applied per voice (linear 0–127).
    void processBlock(float* const* outputChannelData,
                      int           numChannels,
                      int           numSamples) noexcept
    {
        if (numSamples <= 0 || numChannels <= 0 || outputChannelData == nullptr)
            return;

        // Drain the hit queue and assign voices.
        constexpr int kRaw = static_cast<int>(sizeof(PadHitEvent) / sizeof(float));
        float raw[kRaw];
        while (hitQueue_.read(raw, kRaw) == kRaw)
        {
            PadHitEvent ev {};
            static_assert(sizeof(ev) == sizeof(float) * kRaw, "size mismatch");
            __builtin_memcpy(&ev, raw, sizeof(ev));
            assignVoice(ev);
        }

        // Render active voices.
        for (auto& voice : voices_)
        {
            if (!voice.active) continue;

            const int padIdx = voice.padIndex;
            const WavReader& reader = readers_[static_cast<std::size_t>(padIdx)];
            if (!reader.isLoaded())
            {
                voice.active = false;
                continue;
            }

            const float gain = static_cast<float>(voice.velocity) / 127.0f;
            const int remaining = static_cast<int>(
                reader.numFrames() - voice.position);
            const int toRender  = (remaining < numSamples) ? remaining : numSamples;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* out = outputChannelData[ch];
                if (out == nullptr) continue;
                for (int i = 0; i < toRender; ++i)
                {
                    float s = 0.0f;
                    reader.read(voice.position + i, 1, &s, 1);
                    out[i] += s * gain;
                }
            }

            voice.position += toRender;
            if (voice.position >= reader.numFrames())
                voice.active = false;
        }
    }

private:
    // ---- SPSC hit queue (reuse RecordBuffer as a raw-byte carrier) --------
    //
    // RecordBuffer<N> is templated on sample count.
    // sizeof(PadHitEvent) / sizeof(float) == 4 floats (16 bytes on LP64).
    // kQueueSize * 4 gives the float-slot capacity.

    static constexpr unsigned kQueueFloats = kQueueSize * 4u;
    RecordBuffer<kQueueFloats> hitQueue_;

    // ---- Pad definitions (message thread) --------------------------------
    std::array<DrumPad,    kNumPads> pads_;
    std::array<WavReader,  kNumPads> readers_;

    // ---- Voice pool (audio thread) ---------------------------------------
    struct Voice
    {
        bool    active    { false };
        int     padIndex  { 0 };
        int     velocity  { 0 };
        int64_t position  { 0 };
    };

    std::array<Voice, kNumPads * kMaxVoices> voices_;

    std::atomic<double> sampleRate_ { 44100.0 };

    void assignVoice(const PadHitEvent& ev) noexcept
    {
        // Find a free voice slot for this pad, stealing the oldest if full.
        int freeIdx  = -1;
        int stealIdx = -1;

        for (int i = 0; i < static_cast<int>(voices_.size()); ++i)
        {
            const auto& v = voices_[static_cast<std::size_t>(i)];
            if (!v.active && freeIdx < 0)
            {
                freeIdx = i;
            }
            else if (v.active && v.padIndex == ev.padIndex)
            {
                // Prefer stealing an existing same-pad voice for retriggering.
                if (stealIdx < 0) stealIdx = i;
            }
        }

        const int slot = (freeIdx >= 0) ? freeIdx : stealIdx;
        if (slot < 0) return;  // all 64 voices busy — drop

        auto& v     = voices_[static_cast<std::size_t>(slot)];
        v.active    = true;
        v.padIndex  = ev.padIndex;
        v.velocity  = ev.velocity;
        v.position  = 0;
    }
};

} // namespace ssbb
