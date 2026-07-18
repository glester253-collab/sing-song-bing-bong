#pragma once
// MidiRecorder.h — MIDI event recording and basic quantization.
//
// THREAD MODEL:
//   Audio thread   : appendEvent() queues raw MIDI events lock-free.
//   Worker thread  : drainToStore() moves events from the SPSC queue into
//                    the event store; quantize() is called from here.
//   Message thread : quantize(), getEvents(), clearEvents() after recording.
//
// Storage:
//   MidiEvent is a lightweight POD (status, data1, data2, sampleTime).
//   Events are stored in a flat std::vector on the worker/message thread;
//   the audio thread only touches the SPSC ring buffer.

#include "RecordBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ssbb {

// ---- MIDI event POD -------------------------------------------------------

struct MidiEvent
{
    uint8_t status    { 0 };    ///< MIDI status byte (e.g. 0x90 = Note On ch 1).
    uint8_t data1     { 0 };    ///< First data byte (note number or CC number).
    uint8_t data2     { 0 };    ///< Second data byte (velocity or CC value).
    uint8_t reserved  { 0 };    ///< Padding — always 0.
    int64_t sampleTime{ 0 };    ///< Transport position in samples when recorded.
};

// ---- MidiRecorder ---------------------------------------------------------

class MidiRecorder
{
public:
    static constexpr unsigned kQueueFloats = 512u * 3u; // 512 events * 3 floats

    MidiRecorder()  = default;

    // Non-copyable / non-movable (RecordBuffer has atomics).
    MidiRecorder(const MidiRecorder&)            = delete;
    MidiRecorder& operator=(const MidiRecorder&) = delete;
    MidiRecorder(MidiRecorder&&)                 = delete;
    MidiRecorder& operator=(MidiRecorder&&)      = delete;

    // ---- Message / prepare thread ----------------------------------------

    void prepare(double sampleRate, int /*blockSize*/) noexcept
    {
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 44100.0;
    }

    bool isRecording() const noexcept { return recording_; }
    void startRecording() noexcept    { recording_ = true;  }
    void stopRecording()  noexcept    { recording_ = false; }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Queue a MIDI event for later storage.  Drops silently if queue is full.
    void appendEvent(uint8_t status, uint8_t data1, uint8_t data2,
                     int64_t sampleTime) noexcept
    {
        if (!recording_) return;

        // Pack the event into 3 floats via memcpy (no UB, avoids punning).
        struct Pack { uint32_t ab; int64_t t; };
        Pack p{};
        p.ab = (static_cast<uint32_t>(status)  << 24u)
             | (static_cast<uint32_t>(data1)   << 16u)
             | (static_cast<uint32_t>(data2)   <<  8u);
        p.t  = sampleTime;

        float raw[3];
        __builtin_memcpy(raw,     &p.ab, 4);
        __builtin_memcpy(raw + 1, &p.t,  8);

        (void)queue_.write(raw, 3);
    }

    // ---- Worker-thread API -----------------------------------------------

    /// Move all queued events from the ring buffer into the event store.
    void drainToStore()
    {
        float raw[3];
        while (queue_.read(raw, 3) == 3)
        {
            MidiEvent ev{};
            uint32_t ab = 0;
            __builtin_memcpy(&ab,          raw,     4);
            __builtin_memcpy(&ev.sampleTime, raw + 1, 8);
            ev.status = static_cast<uint8_t>((ab >> 24u) & 0xFFu);
            ev.data1  = static_cast<uint8_t>((ab >> 16u) & 0xFFu);
            ev.data2  = static_cast<uint8_t>((ab >>  8u) & 0xFFu);
            store_.push_back(ev);
        }
    }

    // ---- Message-thread API ----------------------------------------------

    /// Quantize note-on/note-off timestamps to the nearest grid subdivision.
    ///
    /// @param bpm          Current tempo in beats per minute.
    /// @param subdivisions Note grid (e.g. 16 = sixteenth-notes).
    /// @param strength     0.0 = no quantization, 1.0 = full snap.
    void quantize(double bpm, int subdivisions, float strength)
    {
        if (bpm <= 0.0 || subdivisions <= 0 || strength <= 0.0f) return;
        const double stepSamples = sampleRate_ * 60.0 / bpm /
                                   static_cast<double>(subdivisions / 4);

        for (auto& ev : store_)
        {
            // Only quantize note-on and note-off.
            const uint8_t type = ev.status & 0xF0u;
            if (type != 0x90u && type != 0x80u) continue;

            const double t        = static_cast<double>(ev.sampleTime);
            const double nearest  = std::round(t / stepSamples) * stepSamples;
            const double moved    = t + (nearest - t) * static_cast<double>(strength);
            ev.sampleTime = static_cast<int64_t>(moved);
        }

        // Re-sort after quantization.
        std::sort(store_.begin(), store_.end(),
                  [](const MidiEvent& a, const MidiEvent& b)
                  { return a.sampleTime < b.sampleTime; });
    }

    const std::vector<MidiEvent>& getEvents() const noexcept { return store_; }

    void clearEvents()
    {
        store_.clear();
    }

private:
    RecordBuffer<kQueueFloats> queue_;
    std::vector<MidiEvent>     store_;
    double                     sampleRate_ { 44100.0 };
    bool                       recording_  { false };
};

} // namespace ssbb
