#pragma once
// PluginHostInterface.h — Evaluation stub for VST3/CLAP/AU plugin hosting.
//
// MILESTONE 5 EVALUATION:
//   VST3, CLAP, and AU plugin hosting requires per-format SDKs with their own
//   licensing terms.  This header defines the interface a concrete host adapter
//   must implement so the rest of the engine can use plugins uniformly.
//
// Licensing notes (evaluate before implementing):
//   - VST3:  Steinberg VST3 SDK, GPLv3 dual-licensed.  Commercial use requires
//            a Steinberg VST3 license.  See https://steinbergmedia.github.io/vst3_doc/
//   - CLAP:  MIT license; no royalty; open standard.
//            See https://cleveraudio.org/
//   - AU:    Apple proprietary; macOS/iOS only; requires Apple developer account.
//
// Recommendation:
//   Start with CLAP for the broadest coverage under a permissive license.
//   Add VST3 after securing the commercial SDK license.
//   AU support is deferred until macOS platform support is added.
//
// THREAD MODEL (when implemented):
//   Message thread: loadPlugin(), unloadPlugin(), setParameter().
//   Audio thread:   processBlock() — must remain lock-free, allocation-free.
//   Worker thread:  scanPlugins() — directory scan and metadata load.
//
// CRASH ISOLATION:
//   Each plugin should run in a subprocess (fork/CreateProcess) and communicate
//   via shared memory or a pipe.  See PluginCrashGuard (future header).
//   Until then, in-process hosting is permitted only for plugins that declare
//   sandbox-safe behaviour.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ssbb {

// ---- Plugin metadata -----------------------------------------------------

enum class PluginFormat : int
{
    Unknown = 0,
    VST3    = 1,
    CLAP    = 2,
    AU      = 3,
};

struct PluginInfo
{
    std::string        id;           ///< Stable unique identifier.
    std::string        name;
    std::string        vendor;
    std::string        version;
    PluginFormat       format    { PluginFormat::Unknown };
    std::filesystem::path path;
    bool               hasMidi  { false };
    bool               hasAudio { true };
    int                numInputs  { 2 };
    int                numOutputs { 2 };
};

// ---- Abstract plugin instance interface ----------------------------------

class IPluginInstance
{
public:
    virtual ~IPluginInstance() = default;

    /// Prepare for audio processing.  Called before the first processBlock().
    virtual bool prepare(double sampleRate, int maxBlockSize) = 0;

    /// Release DSP resources.  Call before unloading.
    virtual void release() = 0;

    /// Set a parameter by index.  Message thread only.
    virtual void setParameter(int index, float value) = 0;
    virtual float getParameter(int index) const = 0;
    virtual int   getNumParameters() const = 0;

    /// Audio thread: process one block.
    /// inputBuffers and outputBuffers are arrays of `numChannels` channel pointers.
    /// MUST NOT: allocate, lock, log, access files/network, throw exceptions.
    virtual void processBlock(const float* const* inputBuffers,
                               float* const*       outputBuffers,
                               int                 numChannels,
                               int                 numSamples) noexcept = 0;

    /// The info struct for this instance.
    virtual const PluginInfo& info() const noexcept = 0;
};

// ---- Abstract plugin host interface --------------------------------------

class IPluginHost
{
public:
    virtual ~IPluginHost() = default;

    /// Scan `directory` for plugins of the supported format(s).
    /// WORKER THREAD.
    virtual std::vector<PluginInfo> scanDirectory(
        const std::filesystem::path& directory) = 0;

    /// Load a plugin from `info.path` and return an instance.
    /// Returns nullptr on failure.  MESSAGE THREAD.
    virtual std::unique_ptr<IPluginInstance> loadPlugin(const PluginInfo& info) = 0;

    /// Unload a plugin instance.  MESSAGE THREAD.
    virtual void unloadPlugin(std::unique_ptr<IPluginInstance> instance) = 0;

    /// True if this host supports the given format.
    virtual bool supportsFormat(PluginFormat format) const noexcept = 0;
};

// ---- Null host (stub — returns no plugins, loads nothing) ----------------
//
// This stub satisfies the interface without linking any SDK.
// Replace with a concrete implementation (ClapHost, Vst3Host) once the
// appropriate SDK license is in place.

class NullPluginHost final : public IPluginHost
{
public:
    std::vector<PluginInfo> scanDirectory(const std::filesystem::path&) override
    {
        return {};   // No plugins found until a real host is implemented.
    }

    std::unique_ptr<IPluginInstance> loadPlugin(const PluginInfo&) override
    {
        return nullptr;
    }

    void unloadPlugin(std::unique_ptr<IPluginInstance>) override {}

    bool supportsFormat(PluginFormat) const noexcept override { return false; }
};

} // namespace ssbb
