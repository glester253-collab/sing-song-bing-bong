#pragma once
// ClipPlayer.h — pre-loaded single-track clip playback.
//
// THREAD MODEL:
//   Worker thread : loadClip() / clearClips()
//   Audio thread  : processBlock()   [NO alloc, NO lock, NO I/O]
//   Message thread: addClip() sets a pending update flag;
//                   the worker thread calls loadClip() to fulfil it.
//
// Each Clip is backed by a WavReader that is fully loaded into RAM before
// the audio thread is allowed to read from it.  The audio thread only reads
// immutable, pre-loaded data via atomic flag checks — no file I/O ever
// happens on the audio thread.
//
// Limitation (Milestone 1): a maximum of kMaxClips slots are supported.
// Slots are reused by clearClips().  This avoids dynamic allocation during
// normal use.

#include "Clip.h"
#include "WavReader.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace ssbb {

class ClipPlayer
{
public:
    static constexpr std::size_t kMaxClips = 32;

    ClipPlayer() = default;

    // Non-copyable / non-movable (contains atomics in SlotState).
    ClipPlayer(const ClipPlayer&)            = delete;
    ClipPlayer& operator=(const ClipPlayer&) = delete;
    ClipPlayer(ClipPlayer&&)                 = delete;
    ClipPlayer& operator=(ClipPlayer&&)      = delete;

    // ---- Message-thread API ----------------------------------------------

    /// Add a clip.  Returns the slot index, or -1 if all slots are full.
    /// The clip is not yet audible: call loadClip(index) from the worker thread.
    int addClip(const Clip& clip)
    {
        for (std::size_t i = 0; i < kMaxClips; ++i)
        {
            int expected = static_cast<int>(SlotState::Empty);
            if (slots_[i].state.compare_exchange_strong(
                    expected,
                    static_cast<int>(SlotState::Pending),
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                slots_[i].clip = clip;
                return static_cast<int>(i);
            }
        }
        return -1;  // no free slot
    }

    /// Update the Clip data for an existing slot without re-loading the audio.
    /// Only safe when the source WAV file has not changed (trim/move edits).
    void updateClip(int slotIndex, const Clip& clip)
    {
        if (slotIndex < 0 || static_cast<std::size_t>(slotIndex) >= kMaxClips) return;
        slots_[static_cast<std::size_t>(slotIndex)].clip = clip;
    }

    /// Remove all clips.  WORKER THREAD: call this before rebuilding the clip list.
    void clearClips()
    {
        for (auto& slot : slots_)
        {
            slot.state.store(static_cast<int>(SlotState::Empty),
                             std::memory_order_release);
            slot.reader.reset();
        }
    }

    // ---- Worker-thread API -----------------------------------------------

    /// Load the WAV file for slot `index` into RAM.
    /// After this returns, the audio thread can play the clip.
    bool loadClip(int slotIndex)
    {
        if (slotIndex < 0 || static_cast<std::size_t>(slotIndex) >= kMaxClips)
            return false;

        auto& slot = slots_[static_cast<std::size_t>(slotIndex)];
        const int st = slot.state.load(std::memory_order_acquire);
        if (st != static_cast<int>(SlotState::Pending) &&
            st != static_cast<int>(SlotState::Ready))
            return false;

        slot.state.store(static_cast<int>(SlotState::Loading),
                         std::memory_order_release);

        const bool ok = slot.reader.load(slot.clip.takePath);

        slot.state.store(ok ? static_cast<int>(SlotState::Ready)
                            : static_cast<int>(SlotState::Empty),
                         std::memory_order_release);
        return ok;
    }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Mix all ready clips into `outBuffer` based on the transport position.
    ///
    /// @param outBuffer         Output interleaved buffer (numChannels * numSamples).
    /// @param numChannels       Number of output channels.
    /// @param numSamples        Block size in frames.
    /// @param blockStartSamples Timeline position at the start of this block (samples).
    void processBlock(float* const* outputChannelData,
                      int           numChannels,
                      int           numSamples,
                      int64_t       blockStartSamples) noexcept
    {
        if (numSamples <= 0 || numChannels <= 0 || outputChannelData == nullptr)
            return;

        for (std::size_t s = 0; s < kMaxClips; ++s)
        {
            auto& slot = slots_[s];
            if (slot.state.load(std::memory_order_acquire) !=
                    static_cast<int>(SlotState::Ready))
                continue;

            const Clip&   clip = slot.clip;
            const int64_t clipStart  = clip.offsetSamples;
            const int64_t clipEnd    = clipStart + clip.activeLengthSamples();

            // Skip if this block doesn't overlap the clip's active region.
            const int64_t blockEnd = blockStartSamples + static_cast<int64_t>(numSamples);
            if (blockEnd <= clipStart || blockStartSamples >= clipEnd)
                continue;

            // Compute overlap region within this block.
            const int64_t overlapStart = (blockStartSamples < clipStart)
                                             ? clipStart : blockStartSamples;
            const int64_t overlapEnd   = (blockEnd < clipEnd) ? blockEnd : clipEnd;

            const int blockOffset   = static_cast<int>(overlapStart - blockStartSamples);
            const int overlapFrames = static_cast<int>(overlapEnd - overlapStart);

            // Source frame inside the WAV file.
            const int64_t srcStart = (overlapStart - clipStart) + clip.trimStartSamples;

            // Mix into each output channel.
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float* out = outputChannelData[ch];
                if (out == nullptr) continue;

                for (int i = 0; i < overlapFrames; ++i)
                {
                    const int64_t srcFrame = srcStart + i;
                    float         sample   = 0.0f;
                    slot.reader.read(srcFrame, 1, &sample, 1);
                    out[blockOffset + i] += sample;
                }
            }
        }
    }

private:
    enum class SlotState : int
    {
        Empty   = 0,
        Pending = 1,
        Loading = 2,
        Ready   = 3
    };

    struct Slot
    {
        Clip              clip;
        WavReader         reader;
        std::atomic<int>  state { static_cast<int>(SlotState::Empty) };

        // Non-copyable because WavReader is non-copyable.
        Slot() = default;
        Slot(const Slot&)            = delete;
        Slot& operator=(const Slot&) = delete;
    };

    std::array<Slot, kMaxClips> slots_;
};

} // namespace ssbb
