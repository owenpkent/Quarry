//
// Checking the arithmetic of recording a picture with a take: the size and bitrate of the
// video, the pacing clock, and how the sound is lined up with it.
//

#ifndef QUARRY_SCREEN_TEST_H
#define QUARRY_SCREEN_TEST_H

#include <cmath>
#include <iostream>
#include <vector>

#include "Sampler/ScreenMath.h"

inline bool screen_test()
{
    namespace sm = quarry::sampler::screenmath;

    int failures = 0;

    const auto check = [&failures](bool condition, const std::string& what)
    {
        if (! condition)
        {
            std::cout << "  FAILED: " << what << std::endl;
            ++failures;
        }
    };

    //==========================================================================
    // Encode size.
    {
        check(sm::encodeSize(1921, 1081) == sm::Size { 1920, 1080 }, "odd sides round down to even");
        check(sm::encodeSize(1920, 1080) == sm::Size { 1920, 1080 }, "even sides are left alone");
        check(sm::encodeSize(3840, 2160) == sm::Size { 3840, 2160 }, "a 4K monitor is not scaled");
        check(sm::encodeSize(1, 3) == sm::Size { 16, 16 }, "tiny sides are raised to the minimum");
        check(sm::encodeSize(17, 400) == sm::Size { 16, 400 }, "the minimum applies to one side alone");
        check(sm::encodeSize(15, 15) == sm::Size { 16, 16 }, "an odd side under the minimum is raised");
    }

    //==========================================================================
    // The copy box.
    {
        const sm::Size encode { 1920, 1080 };

        check(sm::copyExtent({ 1920, 1080 }, { 1920, 1080 }, encode) == sm::Size { 1920, 1080 },
              "a frame the size of the encoder is copied whole");
        check(sm::copyExtent({ 1280, 720 }, { 1280, 720 }, encode) == sm::Size { 1280, 720 },
              "a smaller window is copied whole, to be padded");
        check(sm::copyExtent({ 3000, 2000 }, { 3000, 2000 }, encode) == sm::Size { 1920, 1080 },
              "a larger window is clipped to the encoder");

        // Start at 1920x1080, shrink to 1280x720 so the pool is recreated at that size, then
        // grow to 1600x900: the first frame reports 1600x900 but its surface is still 1280x720.
        check(sm::copyExtent({ 1600, 900 }, { 1280, 720 }, encode) == sm::Size { 1280, 720 },
              "the transition frame after a shrink then grow stays inside the old pool");
        check(sm::copyExtent({ 1600, 900 }, { 1600, 900 }, encode) == sm::Size { 1600, 900 },
              "the frame after the pool is recreated is copied whole");
        check(sm::copyExtent({ 1600, 700 }, { 1280, 720 }, encode) == sm::Size { 1280, 700 },
              "each side is bounded on its own");
        check(sm::copyExtent({ 0, 0 }, { 1280, 720 }, encode) == sm::Size { 0, 0 }, "an empty frame copies nothing");
        check(sm::copyExtent({ -5, 100 }, { 1280, 720 }, encode) == sm::Size { 0, 100 }, "a negative side is never passed on");
    }

    //==========================================================================
    // Bitrate.
    {
        check(sm::bitrate(1920, 1080, 60) == 6220800u, "1080p60 is a twentieth of a bit per pixel");
        check(sm::bitrate(320, 240, 60) == 4000000u, "a small window is held at the floor");
        check(sm::bitrate(3840, 2160, 60) == 24883200u, "4K60 is below the ceiling");
        check(sm::bitrate(7680, 4320, 60) == 30000000u, "8K60 is held at the ceiling");
    }

    //==========================================================================
    // Tick times.
    {
        const juce::int64 freq = 10000000;
        check(sm::tickDue(1000, 0, freq, 60) == 1000, "tick zero is the origin");
        check(sm::tickDue(0, 60, freq, 60) == freq, "sixty ticks is one second");
        check(sm::tickDue(0, 1, freq, 60) == 166666, "one tick is a sixtieth, rounded down");
        check(sm::tickDue(5, 30, freq, 60) == 5 + freq / 2, "thirty ticks is half a second");

        // Two hours at 60 fps on a counter far faster than any real one, where the naive
        // tick * frequency would be 4.3e17 and the split form stays small.
        const juce::int64 twoHours = 2 * 60 * 60 * 60;
        const juce::int64 hugeFreq = 1000000000000LL;
        check(sm::tickDue(0, twoHours, hugeFreq, 60) == 2 * 60 * 60 * hugeFreq,
              "two hours does not overflow even on a very fast counter");
        check(sm::tickDue(0, twoHours - 1, freq, 60) < sm::tickDue(0, twoHours, freq, 60),
              "ticks stay in order at the far end");
    }

    //==========================================================================
    // Where the first block of sound goes.
    {
        const juce::int64 freq = 10000000;
        const double rate = 48000.0;

        // A 10 ms block arriving exactly at the origin began 10 ms before it: drop all 480.
        {
            const auto a = sm::alignFirstBlock(1000000, 1000000, freq, 480, rate);
            check(a.silenceFrames == 0 && a.dropFrames == 480, "a block ending at the origin is dropped whole");
        }

        // The same block arriving 100 ms after the origin began 90 ms in: 4320 frames of silence.
        {
            const auto a = sm::alignFirstBlock(0, freq / 10, freq, 480, rate);
            check(a.silenceFrames == 4320 && a.dropFrames == 0, "a later block gets silence in front");
        }

        // Arriving 4 ms after the origin it began 6 ms before it: drop 288 of 480 frames.
        {
            const auto a = sm::alignFirstBlock(0, freq / 250, freq, 480, rate);
            check(a.silenceFrames == 0 && a.dropFrames == 288, "a block that straddles the origin loses its head");
        }

        // A block that began exactly at the origin needs nothing.
        {
            const auto a = sm::alignFirstBlock(0, freq / 100, freq, 480, rate);
            check(a.silenceFrames == 0 && a.dropFrames == 0, "a block beginning at the origin needs nothing");
        }

        // Far before the origin: never drops more than the block holds.
        {
            const auto a = sm::alignFirstBlock(freq * 10, 0, freq, 480, rate);
            check(a.dropFrames == 480, "the drop is capped at the block length");
        }

        {
            const auto a = sm::alignFirstBlock(0, freq, freq, 0, rate);
            check(a.silenceFrames == 0 && a.dropFrames == 0, "an empty block is a no-op");
        }
    }

    //==========================================================================
    // Catch-up.
    {
        const double rate = 48000.0;
        const juce::int64 margin = 2400;   // stallFillMarginSeconds at 48 kHz

        // Delivering: however far behind the sound is, nothing is inserted.
        check(sm::stallFill(10.0, 10.0, 480000, rate) == 0, "sound level with the picture is left alone");
        check(sm::stallFill(3600.0, 3599.99, 3600 * 48000 - 48000, rate) == 0,
              "a slow clock a whole second behind, still delivering, is never filled");
        check(sm::stallFill(10.0, 9.5, 0, rate) == 0, "exactly half a second without sound is not yet a stall");

        // Stalled: filled to a margin behind the picture, and then kept there.
        check(sm::stallFill(10.0, 9.4, 451200, rate) == 480000 - margin - 451200, "a stall is filled to the margin");
        check(sm::stallFill(10.0, 0.0, 0, rate) == 480000 - margin, "a source that never spoke is filled to the margin");
        check(sm::stallFill(10.0, 9.0, 480000 - margin, rate) == 0, "a track already at the margin gets nothing");
        check(sm::stallFill(10.0, 9.0, 480000 + 96000, rate) == 0, "sound ahead of the picture is never trimmed");
        check(sm::stallFill(1.0, 0.0, 0, 0.0) == 0, "a zero rate is a no-op");

        // Silence the source hands over late is not written twice.
        check(sm::silenceAlreadyWritten(1000, 480, true) == 480, "a silent block inside the credit is left out whole");
        check(sm::silenceAlreadyWritten(100, 480, true) == 100, "a silent block is shortened by what credit is left");
        check(sm::silenceAlreadyWritten(1000, 480, false) == 0, "a block with sound in it is never shortened");
        check(sm::silenceAlreadyWritten(0, 480, true) == 0, "no credit, nothing left out");

        const float zeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float sound[4] = { 0.0f, 0.0f, 1.0e-6f, 0.0f };
        const float* quiet[2] = { zeros, zeros };
        const float* loud[2] = { zeros, sound };
        check(sm::isSilent(quiet, 2, 4), "all zeros is silence");
        check(! sm::isSilent(loud, 2, 4), "one small sample in one channel is not");
    }

    //==========================================================================
    // Downmix and conversion.
    {
        const float a[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
        const float b[4] = { 0.5f, 0.6f, 0.7f, 0.8f };
        float out[8] = {};

        {
            const float* mono[1] = { a };
            sm::downmixToStereo(mono, 1, 4, out);
            check(out[0] == 0.1f && out[1] == 0.1f && out[6] == 0.4f && out[7] == 0.4f, "mono is duplicated");
        }

        {
            const float* stereo[2] = { a, b };
            sm::downmixToStereo(stereo, 2, 4, out);
            check(out[0] == 0.1f && out[1] == 0.5f && out[6] == 0.4f && out[7] == 0.8f,
                  "stereo passes through interleaved");
        }

        {
            // Six channels, centre only: both sides get 0.707 of it, halved.
            const std::vector<float> silent(4, 0.0f);
            const float* six[6] = { silent.data(), silent.data(), a, silent.data(), silent.data(), silent.data() };
            sm::downmixToStereo(six, 6, 4, out);
            check(std::fabs(out[0] - 0.1f * 0.707f * 0.5f) < 1.0e-6f
                      && std::fabs(out[1] - 0.1f * 0.707f * 0.5f) < 1.0e-6f,
                  "a centre channel folds equally into both sides");
        }

        {
            // Eight channels, back and side left only: reaches the left, not the right.
            const std::vector<float> silent(4, 0.0f);
            const float* eight[8] = { silent.data(), silent.data(), silent.data(), silent.data(),
                                      a, silent.data(), b, silent.data() };
            sm::downmixToStereo(eight, 8, 4, out);
            check(std::fabs(out[0] - (0.5f * 0.1f + 0.5f * 0.5f) * 0.5f) < 1.0e-6f && out[1] == 0.0f,
                  "back and side left fold into the left only");
        }

        check(sm::floatToInt16(0.0f) == 0, "zero stays zero");
        check(sm::floatToInt16(1.0f) == 32767, "full scale is 32767");
        check(sm::floatToInt16(-1.0f) == -32767, "negative full scale is -32767");
        check(sm::floatToInt16(3.5f) == 32767, "an over-range sample clips high");
        check(sm::floatToInt16(-3.5f) == -32767, "an over-range sample clips low");
        check(sm::floatToInt16(0.5f) == 16384, "half scale rounds to nearest");
    }

    //==========================================================================
    check(sm::framesPerSecond == 60, "the picture runs at 60 fps");
    check(sm::fifoSeconds == 2.0, "the audio hand-off holds two seconds");

    if (failures == 0)
        std::cout << "  all screen checks passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_SCREEN_TEST_H
