#pragma once
// WavWriter.h — header-only minimal IEEE 754 float-32 WAV writer.
//
// THREAD SAFETY: all methods must be called from the WORKER or MESSAGE thread
// only.  Never call WavWriter methods from the audio thread.
//
// FORMAT: RIFF / WAVE, fmt chunk with AudioFormat = 3 (IEEE_FLOAT),
//         32-bit samples, little-endian, interleaved channels.
//
// USAGE:
//   WavWriter w;
//   if (w.open(path, 44100.0, 1)) {
//       w.write(samples, numSamples);
//       w.close();          // updates RIFF/data chunk sizes and flushes
//   }

#include <cstdint>
#include <cstring>     // std::memcpy
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace ssbb {

class WavWriter
{
public:
    WavWriter()  = default;

    // Non-copyable
    WavWriter(const WavWriter&)            = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // Movable (for optional<> use-cases)
    WavWriter(WavWriter&& o) noexcept
        : file_(std::move(o.file_))
        , numChannels_(o.numChannels_)
        , sampleRate_(o.sampleRate_)
        , dataBytes_(o.dataBytes_)
        , isOpen_(o.isOpen_)
        , failed_(o.failed_)
    {
        o.isOpen_ = false;
    }

    ~WavWriter()
    {
        if (isOpen_) close();
    }

    /// Open a new WAV file.  Returns false on failure.
    /// Must NOT be called from the audio thread.
    bool open(const std::filesystem::path& path,
              double sampleRate, int numChannels) noexcept
    {
        if (isOpen_) close();

        if (path.empty() || !std::isfinite(sampleRate) || sampleRate <= 0.0 ||
            sampleRate > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
            numChannels <= 0 || numChannels > 2)
            return false;

        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_.is_open()) return false;

        numChannels_ = numChannels;
        sampleRate_  = static_cast<uint32_t>(sampleRate);
        dataBytes_   = 0;
        failed_      = false;
        isOpen_      = true;

        writeHeader();
        if (!file_.good())
        {
            failed_ = true;
            close();
            return false;
        }
        return true;
    }

    /// Write interleaved float samples.
    /// numSamples is the total sample count across all channels
    /// (e.g. 512 frames * 1 ch = 512 samples).
    /// Must NOT be called from the audio thread.
    bool write(const float* samples, int numSamples) noexcept
    {
        if (!isOpen_ || samples == nullptr || numSamples <= 0) return false;

        const uint64_t bytes = static_cast<uint64_t>(sizeof(float))
                               * static_cast<uint64_t>(numSamples);
        if (bytes > UINT32_MAX - dataBytes_)
        {
            failed_ = true;
            return false;
        }

        file_.write(reinterpret_cast<const char*>(samples),
                    static_cast<std::streamsize>(bytes));

        if (!file_.good())
        {
            failed_ = true;
            return false;
        }

        dataBytes_ += static_cast<uint32_t>(bytes);
        return true;
    }

    /// Finalize the file: seek back and update RIFF/data chunk sizes, flush.
    /// Must NOT be called from the audio thread.
    void close() noexcept
    {
        if (!isOpen_) return;

        // RIFF chunk size = 36 + dataBytes
        //   4  ("WAVE") + 8 (fmt header) + 16 (fmt body) + 8 (data header)
        //   = 36 bytes before the sample data
        const uint32_t riffSize = 36u + dataBytes_;

        file_.seekp(4);
        writeLE32(riffSize);

        file_.seekp(40);
        writeLE32(dataBytes_);

        file_.flush();
        if (!file_.good())
            failed_ = true;
        file_.close();
        isOpen_    = false;
        dataBytes_ = 0;
    }

    bool isOpen() const noexcept { return isOpen_; }
    bool hasError() const noexcept { return failed_; }

    /// Total number of bytes written to the data chunk so far.
    uint32_t dataBytesWritten() const noexcept { return dataBytes_; }

private:
    // ---- Little-endian helpers ------------------------------------------

    void writeLE16(uint16_t v) noexcept
    {
        const uint8_t b[2] = { static_cast<uint8_t>(v),
                                static_cast<uint8_t>(v >> 8u) };
        file_.write(reinterpret_cast<const char*>(b), 2);
    }

    void writeLE32(uint32_t v) noexcept
    {
        const uint8_t b[4] = { static_cast<uint8_t>(v),
                                static_cast<uint8_t>(v >> 8u),
                                static_cast<uint8_t>(v >> 16u),
                                static_cast<uint8_t>(v >> 24u) };
        file_.write(reinterpret_cast<const char*>(b), 4);
    }

    // ---- WAV header (44 bytes, IEEE float format) -----------------------
    //
    // Offset  Bytes  Contents
    //  0       4     "RIFF"
    //  4       4     fileSize - 8  (placeholder, updated in close())
    //  8       4     "WAVE"
    // 12       4     "fmt "
    // 16       4     16   (fmt subchunk size)
    // 20       2     3    (WAVE_FORMAT_IEEE_FLOAT)
    // 22       2     numChannels
    // 24       4     sampleRate
    // 28       4     byteRate  = sampleRate * numChannels * 4
    // 32       2     blockAlign = numChannels * 4
    // 34       2     32   (bits per sample)
    // 36       4     "data"
    // 40       4     dataSize  (placeholder, updated in close())
    // 44+            samples (little-endian IEEE 754 float32, interleaved)

    void writeHeader() noexcept
    {
        const uint32_t byteRate   = sampleRate_
                                    * static_cast<uint32_t>(numChannels_)
                                    * 4u;
        const uint16_t blockAlign = static_cast<uint16_t>(numChannels_ * 4);

        file_.write("RIFF", 4);
        writeLE32(36u);                          // placeholder
        file_.write("WAVE", 4);
        file_.write("fmt ", 4);
        writeLE32(16u);                          // fmt subchunk size
        writeLE16(3u);                           // WAVE_FORMAT_IEEE_FLOAT
        writeLE16(static_cast<uint16_t>(numChannels_));
        writeLE32(sampleRate_);
        writeLE32(byteRate);
        writeLE16(blockAlign);
        writeLE16(32u);                          // bits per sample
        file_.write("data", 4);
        writeLE32(0u);                           // placeholder
    }

    std::ofstream file_;
    int      numChannels_ { 1 };
    uint32_t sampleRate_  { 44100u };
    uint32_t dataBytes_   { 0u };
    bool     isOpen_      { false };
    bool     failed_      { false };
};

} // namespace ssbb
