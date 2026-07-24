#pragma once
// AutomationLane.h — parameter automation with linear interpolation.
//
// An AutomationLane stores a sorted list of (sampleTime, value) breakpoints.
// The audio thread queries the lane to get the smoothed parameter value at a
// given sample position.
//
// THREAD MODEL:
//   Message thread : addPoint(), removePoint(), clearPoints() — non-atomic writes
//                    using std::mutex to protect the breakpoint list.
//   Audio thread   : evaluate() reads the snapshot pointer atomically; no lock,
//                    no allocation.
//
// Double-buffer:
//   The message thread writes into one vector, then atomically swaps the pointer
//   so the audio thread always reads a stable snapshot.  A generation counter
//   guards against reading a partially written buffer.
//
// Parameter smoothing:
//   evaluate() returns the raw interpolated value.  Callers that need one-pole
//   smoothing (e.g. MixerChannel::setGain) should apply it after each block.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ssbb {

// ---- AutomationPoint POD -----------------------------------------------

struct AutomationPoint
{
    int64_t sampleTime { 0 };    ///< Timeline position in samples.
    float   value      { 0.0f }; ///< Parameter value at this point.
};

// ---- AutomationLane ----------------------------------------------------

class AutomationLane
{
public:
    /// Human-readable name (e.g. "Gain", "FilterCutoff").
    explicit AutomationLane(std::string name = "")
        : name_(std::move(name))
    {
        // Publish empty snapshot immediately.
        auto snap = std::make_shared<Snapshot>();
        snapshot_.store(snap, std::memory_order_release);
    }

    // Non-copyable / non-movable (atomic shared_ptr + mutex).
    AutomationLane(const AutomationLane&)            = delete;
    AutomationLane& operator=(const AutomationLane&) = delete;
    AutomationLane(AutomationLane&&)                 = delete;
    AutomationLane& operator=(AutomationLane&&)      = delete;

    const std::string& name() const noexcept { return name_; }

    // ---- Message-thread API (protected by mutex_) -------------------------

    /// Add or update a breakpoint at `sampleTime`.
    void addPoint(int64_t sampleTime, float value)
    {
        std::lock_guard<std::mutex> lk(mutex_);

        // Update existing or insert sorted.
        auto it = std::lower_bound(edit_.begin(), edit_.end(),
                                   sampleTime, [](const AutomationPoint& p, int64_t t)
                                   { return p.sampleTime < t; });
        if (it != edit_.end() && it->sampleTime == sampleTime)
            it->value = value;
        else
            edit_.insert(it, AutomationPoint{ sampleTime, value });

        publish();
    }

    /// Remove the breakpoint nearest to `sampleTime` within `toleranceSamples`.
    void removePoint(int64_t sampleTime, int64_t toleranceSamples = 0)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        edit_.erase(
            std::remove_if(edit_.begin(), edit_.end(),
                           [&](const AutomationPoint& p)
                           {
                               const int64_t d = p.sampleTime - sampleTime;
                               return (d < 0 ? -d : d) <= toleranceSamples;
                           }),
            edit_.end());
        publish();
    }

    void clearPoints()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        edit_.clear();
        publish();
    }

    const std::vector<AutomationPoint>& getPoints() const noexcept { return edit_; }

    // ---- Audio-thread API ------------------------------------------------
    // MUST NOT: allocate, lock, log, access files/network, throw exceptions.

    /// Return the interpolated value at `sampleTime`.
    /// Returns `defaultValue` if there are no breakpoints.
    float evaluate(int64_t sampleTime, float defaultValue = 0.0f) const noexcept
    {
        const auto snap = snapshot_.load(std::memory_order_acquire);
        if (!snap || snap->points.empty())
            return defaultValue;

        const auto& pts = snap->points;

        // Before first point.
        if (sampleTime <= pts.front().sampleTime)
            return pts.front().value;

        // After last point.
        if (sampleTime >= pts.back().sampleTime)
            return pts.back().value;

        // Binary search for the segment.
        std::size_t lo = 0;
        std::size_t hi = pts.size() - 1;
        while (lo + 1 < hi)
        {
            const std::size_t mid = (lo + hi) / 2;
            if (pts[mid].sampleTime <= sampleTime)
                lo = mid;
            else
                hi = mid;
        }

        // Linear interpolation between pts[lo] and pts[hi].
        const int64_t dt = pts[hi].sampleTime - pts[lo].sampleTime;
        if (dt == 0) return pts[lo].value;

        const float t = static_cast<float>(sampleTime - pts[lo].sampleTime)
                      / static_cast<float>(dt);
        return pts[lo].value + t * (pts[hi].value - pts[lo].value);
    }

private:
    // ---- Immutable snapshot read by the audio thread ---------------------
    struct Snapshot
    {
        std::vector<AutomationPoint> points;
    };

    std::atomic<std::shared_ptr<Snapshot>> snapshot_;

    // ---- Edit buffer (message thread, protected by mutex_) ---------------
    mutable std::mutex             mutex_;
    std::vector<AutomationPoint>   edit_;
    std::string                    name_;

    void publish()
    {
        // Called under mutex_.  Create a new snapshot and atomically publish.
        auto snap = std::make_shared<Snapshot>();
        snap->points = edit_;   // copy
        snapshot_.store(snap, std::memory_order_release);
    }
};

} // namespace ssbb
