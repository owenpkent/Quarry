#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

//
// The arithmetic behind recording a picture with a take: how big the video is, how fast it is
// paced, and how the sound is lined up with it.
//
// Header-only and juce_core only, deliberately, for the same reason as SampleMath.h: the
// recorder that uses this reaches Windows.Graphics.Capture, Direct3D and Media Foundation,
// none of which can be linked into a test runner. Everything here is a pure function of its
// inputs, so Tests/screen_test.h covers it by including this one header.
//
// The sound rules matter more than the picture ones, because this is a music tool. The
// timestamps of the sound come from counting samples and nothing else; the functions here
// only decide how many frames of silence to add or drop at the start, and when a stalled
// source has fallen far enough behind the picture to be filled in.
//

namespace quarry::sampler::screenmath
{

/** Frames per second of the picture. Fixed: a window that changes slowly gets its last frame
    repeated, so the file is constant frame rate and any editor takes it. */
constexpr int framesPerSecond = 60;

/** How much sound the capture thread may be ahead of the worker before it drops. */
constexpr double fifoSeconds = 2.0;

/** The smallest side an encoder is asked for. */
constexpr int minEncodeDimension = 16;

/** A source that has delivered nothing for this long has stalled, and is filled. */
constexpr double catchUpSeconds = 0.5;

/** How far behind the picture a stalled source's track is kept while it is filled. */
constexpr double stallFillMarginSeconds = 0.05;

/** Bitrate bounds, bits per second. */
constexpr double minBitrate = 4.0e6;
constexpr double maxBitrate = 30.0e6;

/**
 * Polls `ready` every couple of milliseconds until it is true, `milliseconds` have passed or
 * `giveUp` is true, whichever comes first. True only when `ready` was seen.
 *
 * Both the stop request and the abandon flag are given up on here, so that a stop pressed while
 * the worker is still waiting for the first frame ends the wait at once instead of after the
 * whole timeout, during which the sound would keep being written to the take.
 */
template <typename Ready, typename GiveUp>
inline bool waitUntilReady (int milliseconds, Ready ready, GiveUp giveUp)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

    while (std::chrono::steady_clock::now() < end && ! giveUp())
    {
        if (ready())
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    return false;
}

//==============================================================================
struct Size
{
    int width = 0;
    int height = 0;

    bool operator== (const Size& other) const noexcept { return width == other.width && height == other.height; }
};

/** One side of the encoded picture: rounded down to even, because H.264 in 4:2:0 cannot
    carry an odd side, and never below the minimum. */
inline int encodeDimension (int source) noexcept
{
    return std::max (minEncodeDimension, source & ~1);
}

inline Size encodeSize (int sourceWidth, int sourceHeight) noexcept
{
    return { encodeDimension (sourceWidth), encodeDimension (sourceHeight) };
}

/** About a twentieth of a bit per pixel per frame, held between 4 and 30 Mbps. */
inline juce::uint32 bitrate (int width, int height, int fps) noexcept
{
    const auto wanted = (double) width * (double) height * (double) fps * 0.05;
    return (juce::uint32) std::lround (std::clamp (wanted, minBitrate, maxBitrate));
}

/**
 * When tick `tick` is due, in counter ticks, as origin + tick * frequency / fps.
 *
 * Split into whole seconds and a remainder so the product never gets large: a two hour take
 * at 60 fps is 432000 ticks, and times a 10 MHz counter that is already 4e12, but a counter
 * in the gigahertz would not leave much room for the naive form.
 */
inline juce::int64 tickDue (juce::int64 origin, juce::int64 tick, juce::int64 qpcFrequency, int fps) noexcept
{
    const juce::int64 rate = fps;
    return origin + (tick / rate) * qpcFrequency + ((tick % rate) * qpcFrequency) / rate;
}

/**
 * How much of a captured frame to copy into the encoder's texture, from the top left corner.
 *
 * The frame reports the size of the window's content, but its surface keeps the size the
 * capture pool was created with until the pool is recreated, and that happens only after the
 * frame has been handled. So right after a window grows, the content can be larger than the
 * surface it sits in. A copy box that reaches past the source texture is undefined in
 * Direct3D, so the extent is bounded by all three: the content, the surface and the
 * destination. Either side of the result may be zero, and then there is nothing to copy.
 */
inline Size copyExtent (Size content, Size source, Size destination) noexcept
{
    return { std::max (0, std::min ({ content.width, source.width, destination.width })),
             std::max (0, std::min ({ content.height, source.height, destination.height })) };
}

//==============================================================================
/** What to do with the first block of sound so that it lands where it belongs. */
struct StartAlignment
{
    juce::int64 silenceFrames = 0;   // write this much silence before the block
    juce::int64 dropFrames = 0;      // or skip this much of the head of the block
};

/**
 * Lines the first block after the origin up with the picture's clock.
 *
 * `arrival` is when the block was handed to us and is taken to be when it ended, which is
 * how a capture thread that has just been woken sees it. A block that began after the
 * origin gets silence in front of it; one that began before gets its head dropped, because
 * the picture has no frame to go with it.
 */
inline StartAlignment alignFirstBlock (juce::int64 originQpc, juce::int64 arrivalQpc, juce::int64 qpcFrequency,
                                       juce::int64 blockFrames, double sampleRate) noexcept
{
    StartAlignment result;

    if (qpcFrequency <= 0 || blockFrames <= 0 || sampleRate <= 0.0)
        return result;

    const auto endOffset = std::llround ((double) (arrivalQpc - originQpc) / (double) qpcFrequency * sampleRate);
    const auto startOffset = (juce::int64) endOffset - blockFrames;

    if (startOffset > 0)
        result.silenceFrames = startOffset;
    else if (startOffset < 0)
        result.dropFrames = std::min (-startOffset, blockFrames);

    return result;
}

/**
 * Lines a block up with the picture's clock by when it began, not when it was handed over.
 *
 * `startQpc` is when the block's first frame was captured, in counter ticks. This is what
 * alignFirstBlock does when the arrival cannot be trusted to be the end of the block, which is
 * so for silence a stream makes up after an idle gap: the stream hands over the whole gap in a
 * burst when sound returns, and the burst arrives long after the time it stands for. Dating
 * each chunk by its own capture time puts the part that predates the origin before it, where
 * its head is dropped, and the rest where it belongs.
 */
inline StartAlignment alignFirstBlockAt (juce::int64 originQpc, juce::int64 startQpc, juce::int64 qpcFrequency,
                                         juce::int64 blockFrames, double sampleRate) noexcept
{
    StartAlignment result;

    if (qpcFrequency <= 0 || blockFrames <= 0 || sampleRate <= 0.0)
        return result;

    const auto startOffset = (juce::int64) std::llround ((double) (startQpc - originQpc) / (double) qpcFrequency * sampleRate);

    if (startOffset > 0)
        result.silenceFrames = startOffset;
    else if (startOffset < 0)
        result.dropFrames = std::min (-startOffset, blockFrames);

    return result;
}

/** When a block began, in counter ticks, for a source that cannot say: it is taken to have
    ended at `arrivalQpc`, which is how a capture thread that has just been woken sees it. */
inline juce::int64 blockStartFromArrival (juce::int64 arrivalQpc, juce::int64 blockFrames, double sampleRate,
                                          juce::int64 qpcFrequency) noexcept
{
    if (sampleRate <= 0.0)
        return arrivalQpc;

    return arrivalQpc - (juce::int64) std::llround ((double) blockFrames / sampleRate * (double) qpcFrequency);
}

/** Converts the 100 ns units WASAPI reports capture times in to counter ticks. Split into
    seconds and a remainder so the product stays small. */
inline juce::int64 qpcFrom100ns (juce::int64 time100ns, juce::int64 qpcFrequency) noexcept
{
    return (time100ns / 10000000) * qpcFrequency + ((time100ns % 10000000) * qpcFrequency) / 10000000;
}

/**
 * Frames of silence to insert because the source has stopped delivering.
 *
 * A stall is a source that has handed over nothing for half a second, not sound that is
 * merely behind. A sound card whose clock runs a little slower than the system's falls behind
 * steadily while delivering without a break, and filling that would put silence into the
 * middle of the music an hour or so into a take. So nothing is inserted while blocks arrive.
 *
 * Once stalled, the track is kept a small margin behind the picture, tick by tick, so that
 * whatever arrives next lands close to where it belongs. Sound that runs ahead is never
 * trimmed: samples are counted, not edited.
 */
inline juce::int64 stallFill (double videoSeconds, double lastDeliverySeconds, juce::int64 audioFrames,
                              double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || videoSeconds - lastDeliverySeconds <= catchUpSeconds)
        return 0;

    const auto target = (juce::int64) std::llround ((videoSeconds - stallFillMarginSeconds) * sampleRate);
    return target > audioFrames ? target - audioFrames : 0;
}

/**
 * How much of the head of a block to leave out because it is silence already written.
 *
 * Both loopback streams close a gap after the fact: when sound returns they first hand over
 * the silence that was missed, in one burst, and only then the sound. stallFill has by then
 * written that silence itself, as it happened, and writing it twice would leave everything
 * after it late by the length of the gap. `credit` is how much stallFill wrote, in the
 * source's frames. Only a block that is all zeros is ever shortened; a block with any sound
 * in it passes whole, and ends the credit.
 */
inline juce::int64 silenceAlreadyWritten (juce::int64 credit, int blockFrames, bool blockIsSilent) noexcept
{
    if (! blockIsSilent || credit <= 0 || blockFrames <= 0)
        return 0;

    return std::min (credit, (juce::int64) blockFrames);
}

/** True when every sample of the block is exactly zero, which is what padded silence is. */
inline bool isSilent (const float* const* channels, int numChannels, int numFrames) noexcept
{
    for (int c = 0; c < numChannels; ++c)
        for (int i = 0; i < numFrames; ++i)
            if (channels[c][i] != 0.0f)
                return false;

    return true;
}

//==============================================================================
/** Float to 16 bit with clipping. Full scale maps to 32767, so the result is never -32768
    and negating it never overflows. */
inline juce::int16 floatToInt16 (float value) noexcept
{
    value = std::clamp (value, -1.0f, 1.0f);
    return (juce::int16) std::lrintf (value * 32767.0f);
}

/**
 * Folds planar channels to interleaved stereo: one channel is duplicated, two pass through,
 * more use the usual layout (front left and right, centre, low frequency, back, side) with
 * the centre and surrounds folded into the front pair. The low frequency channel is left out,
 * as a stereo downmix does. May exceed full scale, which floatToInt16 then clips.
 */
inline void downmixToStereo (const float* const* channels, int numChannels, int numFrames, float* interleaved) noexcept
{
    if (numChannels <= 0 || numFrames <= 0 || channels == nullptr)
        return;

    const auto count = std::min (numChannels, 8);

    for (int i = 0; i < numFrames; ++i)
    {
        float left, right;

        if (count == 1)
        {
            left = right = channels[0][i];
        }
        else if (count == 2)
        {
            left = channels[0][i];
            right = channels[1][i];
        }
        else
        {
            const auto centre = channels[2][i];
            left = channels[0][i] + 0.707f * centre;
            right = channels[1][i] + 0.707f * centre;

            if (count > 5)
            {
                left += 0.5f * channels[4][i];
                right += 0.5f * channels[5][i];
            }

            if (count > 7)
            {
                left += 0.5f * channels[6][i];
                right += 0.5f * channels[7][i];
            }

            left *= 0.5f;
            right *= 0.5f;
        }

        interleaved[2 * i] = left;
        interleaved[2 * i + 1] = right;
    }
}

} // namespace quarry::sampler::screenmath
