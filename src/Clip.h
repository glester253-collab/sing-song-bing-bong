#pragma once
// Clip.h — non-destructive edit descriptor.
//
// A Clip references a take file on disk (via path) and stores trim/offset
// values.  Edits never touch the source file — they adjust which portion of
// the take is audible and where it sits on the timeline.
//
// All methods are constexpr/noexcept and have no external dependencies.

#include <cstdint>
#include <filesystem>

namespace ssbb {

struct Clip
{
    /// Path to the immutable source take WAV file.
    std::filesystem::path takePath;

    /// Timeline position: samples from the start of the session where the
    /// clip's (trimmed) audio begins.
    int64_t offsetSamples       { 0 };

    /// Samples trimmed from the start of the source file.
    int64_t trimStartSamples    { 0 };

    /// Samples trimmed from the end of the source file.
    int64_t trimEndSamples      { 0 };

    /// Total length of the source take in samples (recorded, never changes).
    int64_t sourceLengthSamples { 0 };

    // ---- Non-destructive edits ------------------------------------------

    /// Move the in-point: trim samples from the beginning of the source.
    /// The source file is not modified.
    void trimStart(int64_t newTrimStart) noexcept
    {
        trimStartSamples = newTrimStart;
    }

    /// Move the out-point: trim samples from the end of the source.
    /// The source file is not modified.
    void trimEnd(int64_t newTrimEnd) noexcept
    {
        trimEndSamples = newTrimEnd;
    }

    /// Reposition the clip on the timeline without changing its content.
    void moveTo(int64_t newOffset) noexcept
    {
        offsetSamples = newOffset;
    }

    // ---- Queries --------------------------------------------------------

    /// Audible length of the clip after both trim operations are applied.
    /// Returns 0 if the trims consume more than the source length.
    [[nodiscard]]
    int64_t activeLengthSamples() const noexcept
    {
        const int64_t active =
            sourceLengthSamples - trimStartSamples - trimEndSamples;
        return (active > 0) ? active : 0;
    }

    /// First source sample that is audible (after trimStart).
    [[nodiscard]]
    int64_t sourceStartSample() const noexcept { return trimStartSamples; }

    /// One-past-last source sample that is audible (before trimEnd).
    [[nodiscard]]
    int64_t sourceEndSample() const noexcept
    {
        return sourceLengthSamples - trimEndSamples;
    }
};

} // namespace ssbb
