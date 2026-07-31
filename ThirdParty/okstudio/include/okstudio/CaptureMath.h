#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// Pure capture arithmetic: size budgets, sample-rate negotiation, meter ballistics.
// No devices, no files, nothing past juce_core, so the kit's own test executable
// (juce_core + juce_events and no more) can link it. Everything that touches a real
// audio device lives in AudioCapture.h, which does reach juce_audio_devices.
namespace okstudio::capture
{

/** Where a capture takes its audio from. */
enum class SourceKind
{
    input,   //< a real input: microphone, line in, an interface channel
    loopback //< what the machine is already playing. WASAPI loopback, Windows only.
};

/** The WAV sample formats we write. 32 means IEEE float: that is what JUCE's
    WavAudioFormat produces when asked for 32 bits per sample, not 32-bit int. */
enum class BitDepth
{
    int16   = 16,
    int24   = 24,
    float32 = 32
};

constexpr int bitsPerSample(BitDepth depth) noexcept { return static_cast<int>(depth); }
constexpr int bytesPerSample(BitDepth depth) noexcept { return bitsPerSample(depth) / 8; }
constexpr bool isFloat(BitDepth depth) noexcept { return depth == BitDepth::float32; }

constexpr bool isSupportedBitDepth(int bits) noexcept { return bits == 16 || bits == 24 || bits == 32; }

/** Recording more channels than you meant to is the quiet way to fill a disk, so an
    unspecified channel count means stereo, not "everything the interface has". */
constexpr int defaultChannelCount = 2;

//==============================================================================
// Size and duration budgets

/** Bytes of WAV audio per second of wall clock, header excluded (44 bytes, or 92 once
    a take passes 4 GB and upgrades to RF64: not worth modelling). */
inline double bytesPerSecond(double sampleRate, int channels, BitDepth depth) noexcept
{
    return juce::jmax(0.0, sampleRate) * juce::jmax(0, channels) * bytesPerSample(depth);
}

inline juce::int64 estimatedBytes(double seconds, double sampleRate, int channels, BitDepth depth) noexcept
{
    return (juce::int64) std::llround(juce::jmax(0.0, seconds) * bytesPerSecond(sampleRate, channels, depth));
}

/** How long you could record into `bytes`. 0 when the format would produce no data. */
inline double secondsForBytes(juce::int64 bytes, double sampleRate, int channels, BitDepth depth) noexcept
{
    const auto rate = bytesPerSecond(sampleRate, channels, depth);
    return rate > 0.0 ? (double) juce::jmax((juce::int64) 0, bytes) / rate : 0.0;
}

/** Why a capture ended. */
enum class StopReason
{
    requested,     //< someone called stop()
    sizeLimit,     //< Limits::maxBytes
    durationLimit, //< Limits::maxSeconds
    diskFull,      //< Limits::minFreeBytes
    deviceLost     //< the device went away underneath us
};

/** Guard rails, all optional. The free-space floor is the one that matters: a capture
    left running is otherwise bounded only by the size of the volume. */
struct Limits
{
    /** Stop past this many bytes written. 0 = no cap. */
    juce::int64 maxBytes = 0;
    /** Stop past this many seconds. 0 = no cap. */
    double maxSeconds = 0.0;
    /** Stop before free space falls below this. 0 = no floor. */
    juce::int64 minFreeBytes = 512ll * 1024 * 1024;
};

/** The reason to stop now, if there is one. `freeBytesOnVolume` may be negative, which
    is how juce::File reports "could not tell", and is treated as no reason to stop. */
inline std::optional<StopReason> limitReached(const Limits& limits, juce::int64 bytesWritten, double secondsElapsed,
                                              juce::int64 freeBytesOnVolume) noexcept
{
    if (limits.maxBytes > 0 && bytesWritten >= limits.maxBytes)
        return StopReason::sizeLimit;

    if (limits.maxSeconds > 0.0 && secondsElapsed >= limits.maxSeconds)
        return StopReason::durationLimit;

    if (limits.minFreeBytes > 0 && freeBytesOnVolume >= 0 && freeBytesOnVolume <= limits.minFreeBytes)
        return StopReason::diskFull;

    return std::nullopt;
}

inline juce::String describe(StopReason reason)
{
    switch (reason)
    {
        case StopReason::requested:     return "Stopped";
        case StopReason::sizeLimit:     return "Stopped: size limit reached";
        case StopReason::durationLimit: return "Stopped: time limit reached";
        case StopReason::diskFull:      return "Stopped: the drive is nearly full";
        case StopReason::deviceLost:    return "Stopped: the audio device disappeared";
    }
    return "Stopped";
}

//==============================================================================
// Opening a device

/** Loopback runs at whatever rate the endpoint is already mixing at. Asking for a
    different one does not resample, it fails to open, so the rate is read-only for
    a loopback source and the UI should say so rather than offer a dead menu. */
constexpr bool sampleRateIsFixed(SourceKind kind) noexcept { return kind == SourceKind::loopback; }

/** The rate to open with, given what the device offers.

    `requested` of 0 means "pick something sane": 48k if it is on the list, else 44.1k,
    else whatever comes first. A requested rate that is not available falls back to the
    nearest one rather than failing, so a saved setting from another machine never blocks
    a capture. A loopback source ignores `requested` entirely. */
inline double chooseSampleRate(const std::vector<double>& available, double requested, SourceKind kind)
{
    if (available.empty())
        return juce::jmax(0.0, requested);

    const auto has = [&available](double rate) {
        return std::find(available.begin(), available.end(), rate) != available.end();
    };

    if (sampleRateIsFixed(kind) || requested <= 0.0)
    {
        if (has(48000.0)) return 48000.0;
        if (has(44100.0)) return 44100.0;
        return available.front();
    }

    auto best = available.front();
    for (const auto rate : available)
        if (std::abs(rate - requested) < std::abs(best - requested))
            best = rate;

    return best;
}

/** Channels to open, clamped to what exists. `requested` of 0 means stereo, or mono on
    a device that only has one input. */
inline int chooseChannelCount(int availableOnDevice, int requested) noexcept
{
    if (availableOnDevice <= 0)
        return 0;

    const auto wanted = requested > 0 ? requested : defaultChannelCount;
    return juce::jlimit(1, availableOnDevice, wanted);
}

//==============================================================================
// Metering

/** Peak meter ballistics: rises instantly to the block's peak, falls at `decayDbPerSecond`.
    Without the fall a meter flickers too fast to read; without the instant rise it misses
    the transient that tells you the source is live. */
inline float decayedPeak(float previous, float blockPeak, double blockSeconds, double decayDbPerSecond = 24.0)
{
    if (blockPeak >= previous)
        return blockPeak;

    const auto factor = (float) std::pow(10.0, -(decayDbPerSecond * juce::jmax(0.0, blockSeconds)) / 20.0);
    return juce::jmax(blockPeak, previous * factor);
}

inline float gainToDecibels(float gain, float floorDb = -100.0f)
{
    return gain > 0.0f ? juce::jmax(floorDb, (float) (20.0 * std::log10((double) gain))) : floorDb;
}

/** 0 to 1 for drawing a meter: `floorDb` and below reads empty, unity gain reads full. */
inline float meterFraction(float gain, float floorDb = -60.0f)
{
    if (floorDb >= 0.0f)
        return juce::jlimit(0.0f, 1.0f, gain);

    return juce::jlimit(0.0f, 1.0f, (gainToDecibels(gain, floorDb) - floorDb) / -floorDb);
}

/** True when nothing loud enough to be real audio has arrived since the capture started.
    The usual cause is the wrong source selected, which is otherwise invisible until
    playback, by which point the take is gone. */
inline bool looksSilent(float peakSinceStart, double secondsElapsed, double graceSeconds = 3.0,
                        float thresholdDb = -60.0f)
{
    return secondsElapsed >= graceSeconds && gainToDecibels(peakSinceStart) < thresholdDb;
}

//==============================================================================

/** "2:07", "1:02:03". juce::RelativeTime rounds to "2 mins", which is no use while
    something is recording. */
inline juce::String formatDuration(double seconds)
{
    const auto total   = (juce::int64) std::floor(juce::jmax(0.0, seconds));
    const auto hours   = total / 3600;
    const auto minutes = (total / 60) % 60;
    const auto secs    = total % 60;

    juce::String out;
    if (hours > 0)
        out << hours << ":" << juce::String(minutes).paddedLeft('0', 2);
    else
        out << minutes;

    return out + ":" + juce::String(secs).paddedLeft('0', 2);
}

} // namespace okstudio::capture
