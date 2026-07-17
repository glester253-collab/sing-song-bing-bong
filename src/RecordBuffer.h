#pragma once
// RecordBuffer.h — lock-free SPSC ring buffer for float audio samples.
//
// AUDIO THREAD rules: write() is safe to call from the audio callback.
//   - No allocation (buffer is a value member, sized at compile time).
//   - No locks (head_ / tail_ are std::atomic with acquire/release ordering).
//   - If the buffer is full, write() drops the block and returns false.
//
// WORKER THREAD: read() drains samples into a caller-supplied output buffer.
//
// Template parameter Capacity MUST be a power of 2 (enforced by static_assert).
// Default 1<<18 = 262 144 samples ≈ 5.9 s at 44.1 kHz.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>  // std::min

namespace ssbb {

template<std::size_t Capacity = (1u << 18u)>
class RecordBuffer
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1u)) == 0u,
                  "RecordBuffer Capacity must be a power of 2 and at least 2");

    static constexpr std::size_t kMask = Capacity - 1u;

public:
    // Compiler-generated default constructor zero-initialises the atomic members.
    RecordBuffer() noexcept = default;

    // Non-copyable / non-movable: the ring buffer is a value resource.
    RecordBuffer(const RecordBuffer&)            = delete;
    RecordBuffer& operator=(const RecordBuffer&) = delete;
    RecordBuffer(RecordBuffer&&)                 = delete;
    RecordBuffer& operator=(RecordBuffer&&)      = delete;

    // ---- AUDIO THREAD (producer) ----------------------------------------
    //
    // Tries to write `count` samples from `samples[0..count-1]` into the
    // buffer.  Returns true on success, false if there is not enough free
    // space (the block is silently dropped — no partial writes).
    //
    // Memory ordering:
    //   - tail_ is loaded with relaxed (producer owns it, no sync needed).
    //   - head_ is loaded with acquire to see the latest consumer advance.
    //   - tail_ is stored with release so the consumer sees the new data.
    [[nodiscard]]
    bool write(const float* samples, int count) noexcept
    {
        if (count <= 0) return true;

        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        // Free slots (unsigned subtraction wraps correctly with power-of-2 mask).
        const std::size_t free = Capacity - (tail - head);

        if (static_cast<std::size_t>(count) > free)
            return false;  // drop — buffer full

        for (int i = 0; i < count; ++i)
            buffer_[(tail + static_cast<std::size_t>(i)) & kMask] = samples[i];

        tail_.store(tail + static_cast<std::size_t>(count),
                    std::memory_order_release);
        return true;
    }

    // ---- WORKER THREAD (consumer) ----------------------------------------
    //
    // Reads up to `maxCount` samples into `samples[0..returned-1]`.
    // Returns the number of samples actually read (0 if the buffer is empty).
    //
    // Memory ordering:
    //   - head_ is loaded with relaxed (consumer owns it).
    //   - tail_ is loaded with acquire to see the latest producer advance.
    //   - head_ is stored with release so the producer sees the advance.
    int read(float* samples, int maxCount) noexcept
    {
        if (maxCount <= 0) return 0;

        const std::size_t head  = head_.load(std::memory_order_relaxed);
        const std::size_t tail  = tail_.load(std::memory_order_acquire);
        const std::size_t avail = tail - head;  // wraps correctly

        const std::size_t toRead =
            std::min(avail, static_cast<std::size_t>(maxCount));

        for (std::size_t i = 0; i < toRead; ++i)
            samples[i] = buffer_[(head + i) & kMask];

        head_.store(head + toRead, std::memory_order_release);
        return static_cast<int>(toRead);
    }

    // ---- Status queries (any thread, approximate) -----------------------

    /// Samples available for reading (snapshot — may be stale by the time
    /// the caller acts on the result).
    std::size_t availableRead() const noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return tail - head;
    }

    /// Free slots available for writing (approximate).
    std::size_t availableWrite() const noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return Capacity - (tail - head);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Consumer (read) index — advanced only by the worker thread.
    // Aligned to its own cache line to prevent false sharing with tail_.
    alignas(64) std::atomic<std::size_t> head_ { 0u };

    // Producer (write) index — advanced only by the audio thread.
    alignas(64) std::atomic<std::size_t> tail_ { 0u };

    // Sample storage — a plain array so no heap allocation is ever needed.
    // Laid out after the atomics so head_/tail_ each occupy a full cache line.
    float buffer_[Capacity] {};
};

} // namespace ssbb
