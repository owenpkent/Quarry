//
// Checking the arithmetic of recording a picture with a take: the size and bitrate of the
// video, the pacing clock, and how the sound is lined up with it.
//

#ifndef QUARRY_SCREEN_TEST_H
#define QUARRY_SCREEN_TEST_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>
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
    // Stopping while the picture is still starting up, or finishing.
    {
        using Clock = std::chrono::steady_clock;
        const auto millisecondsSince = [](Clock::time_point from)
        {
            return (int) std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - from).count();
        };

        // The wait is cut short by a stop request, and by nothing else but its own timeout.
        // Timeouts here are long and the bounds generous: only order and counts are exact.
        {
            std::atomic<bool> stop { false };
            const auto begun = Clock::now();
            std::thread stopper([&stop] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); stop.store(true); });
            const auto ready = sm::waitUntilReady(30000, [] { return false; }, [&stop] { return stop.load(); });
            stopper.join();
            check(! ready, "a wait that is stopped does not report a frame");
            check(millisecondsSince(begun) < 5000, "a stop request ends the first frame wait long before its timeout");
        }

        {
            std::atomic<bool> frame { false };
            std::thread drawer([&frame] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); frame.store(true); });
            const auto ready = sm::waitUntilReady(2000, [&frame] { return frame.load(); }, [] { return false; });
            drawer.join();
            check(ready, "a frame that arrives ends the wait with true");
        }

        {
            const auto begun = Clock::now();
            const auto ready = sm::waitUntilReady(40, [] { return false; }, [] { return false; });
            check(! ready && millisecondsSince(begun) >= 35, "with no frame and no stop the wait runs its timeout");
        }

        // The order Stop uses, against a worker that is slow to finish: the sound stops being
        // accepted the moment Stop is requested, so what the slow shutdown takes is not kept.
        {
            std::mutex gate;   // makes "accepted, then counted" one step, so the stop boundary is exact
            bool accepting = true;
            std::atomic<bool> stopRequested { false };
            std::atomic<bool> running { true };
            std::atomic<int> kept { 0 };

            // Stands in for the capture thread: a block counts only while it is accepted.
            std::thread source([&]
            {
                while (running.load())
                {
                    {
                        std::lock_guard<std::mutex> lock(gate);

                        if (accepting)
                            kept.fetch_add(1);
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });

            // Stands in for the worker: still waiting for a first frame that never comes, then
            // slow to finalise.
            std::thread worker([&]
            {
                sm::waitUntilReady(30000, [] { return false; }, [&stopRequested] { return stopRequested.load(); });
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            const auto begun = Clock::now();
            {
                std::lock_guard<std::mutex> lock(gate);
                accepting = false;
            }

            const auto atStop = kept.load();
            stopRequested.store(true);

            worker.join();
            const auto joinMs = millisecondsSince(begun);
            const auto afterJoin = kept.load();
            running.store(false);
            source.join();

            check(atStop > 0, "the modelled source delivered before the stop");
            check(afterJoin == atStop, "no sound is kept after the stop, however long finalising takes");
            check(joinMs < 5000, "the worker is joined long before the startup timeout");
        }
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
    // An idle gap that spans the start, closed by silence the stream makes up.
    {
        const juce::int64 freq = 10000000;
        const double rate = 48000.0;

        check(sm::qpcFrom100ns(10000000, freq) == freq, "a second in 100 ns units is a second of ticks");
        check(sm::qpcFrom100ns(15, 3000000) == 4, "the conversion rounds down");
        check(sm::qpcFrom100ns(36000000000LL, 3000000) == 10800000000LL, "an hour on a 3 MHz counter converts exactly");
        check(sm::blockStartFromArrival(freq, 480, rate, freq) == freq - 100000, "a 10 ms block ends on arrival");

        // The packet before the gap ended 10 ms before the origin; sound returns at +400 ms, and
        // the stream hands over the 410 ms it missed as chunks of 9600, 9600 and 480 frames.
        const juce::int64 chunks[3] = { 9600, 9600, 480 };
        const juce::int64 origin = 5 * freq;
        const juce::int64 resume = origin + freq * 4 / 10;
        const juce::int64 gapFrames = chunks[0] + chunks[1] + chunks[2];

        // What the stream reports: each chunk dated by counting back from the packet after it.
        juce::int64 start = resume - (juce::int64) std::llround((double) gapFrames / rate * (double) freq);
        const auto firstChunkStart = start;
        check(firstChunkStart == origin - freq / 100, "the first chunk begins 10 ms before the origin");

        const auto align = sm::alignFirstBlockAt(origin, firstChunkStart, freq, chunks[0], rate);
        check(align.silenceFrames == 0 && align.dropFrames == 480, "the part of the gap before the origin is dropped");

        // Everything written ahead of the resumed sound, and where that puts it.
        const auto soundAt = align.silenceFrames + gapFrames - align.dropFrames;
        check(soundAt == 19200, "resumed sound lands at +400 ms");

        // The old rule stamped the first chunk with its delivery, and put it at +610 ms.
        const auto oldStart = sm::blockStartFromArrival(resume, chunks[0], rate, freq);
        const auto old = sm::alignFirstBlock(origin, resume, freq, chunks[0], rate);
        check(oldStart > origin && old.silenceFrames == 9600, "stamped on delivery, the first chunk gets 9600 frames in front");
        check(old.silenceFrames + gapFrames == 29280, "which is what put the sound at +610 ms");

        // That is under the stall threshold, so the stall rule leaves it alone.
        check(sm::stallFill(0.4, 0.0, soundAt, rate) == 0, "no stall fill has happened by +400 ms");

        // The next chunks follow the first without a gap or an overlap in their own times.
        start += (juce::int64) std::llround((double) chunks[0] / rate * (double) freq);
        check(start == origin + freq * 19 / 100,
              "the second chunk begins where the first ended");

        // A longer gap: the last packet ended 500 ms before the origin and sound returns at
        // +400 ms, so 900 ms is made up as chunks of 9600 (four times) and 4800, the first
        // four wholly or partly before the origin. Chunks that end by the origin are left out
        // (blockPredatesOrigin) and the first one kept is trimmed.
        {
            const juce::int64 longChunks[5] = { 9600, 9600, 9600, 9600, 4800 };
            const juce::int64 total = 43200;
            juce::int64 chunkStart = resume - (juce::int64) std::llround((double) total / rate * (double) freq);
            check(chunkStart == origin - freq / 2, "the long gap begins 500 ms before the origin");

            juce::int64 written = 0;
            bool aligned = false;
            int left = 0;

            for (const auto frames : longChunks)
            {
                if (! aligned && sm::blockPredatesOrigin(origin, chunkStart, freq, frames, rate))
                {
                    ++left;
                }
                else
                {
                    if (! aligned)
                    {
                        const auto a = sm::alignFirstBlockAt(origin, chunkStart, freq, frames, rate);
                        written += a.silenceFrames - a.dropFrames;
                        aligned = true;
                    }

                    written += frames;
                }

                chunkStart += (juce::int64) std::llround((double) frames / rate * (double) freq);
            }

            check(left == 2, "the two chunks wholly before the origin are left out");
            check(written == 19200, "resumed sound lands at +400 ms after a gap longer than one chunk");
        }

        // The ingress trim and the alignment share one rounding rule: the head the trim takes
        // off the straddling chunk is the 480 frames the alignment would have dropped.
        check(sm::framesBeforeOrigin(origin, origin - freq / 100, freq, 9600, rate) == 480, "the trim takes the 10 ms before the origin");
        check(sm::framesBeforeOrigin(origin, origin, freq, 9600, rate) == 0, "nothing is trimmed from a block at the origin");
        check(sm::framesBeforeOrigin(origin, origin - freq, freq, 480, rate) == 480, "the trim is capped at the block length");
        check(sm::framesBeforeOrigin(origin, origin - freq, 0, 480, rate) == 0, "a zero counter frequency trims nothing");

        // A gap longer than the stall threshold. The last packet ended 100 ms before the origin
        // and sound returns at +650 ms, so 750 ms (36000 frames) is made up as chunks of 9600,
        // 9600, 9600 and 7200. By its +633 ms tick the worker has already filled 28000 frames
        // of stall silence, so the stream has started and 28000 is owed back. The first chunk
        // spans -100 ms to +100 ms: its 4800 early frames are trimmed first, and only then does
        // the credit look at what is left.
        {
            const juce::int64 backfill[4] = { 9600, 9600, 9600, 7200 };
            const auto resumeLong = origin + freq * 65 / 100;
            auto chunkStart = resumeLong - (juce::int64) std::llround(36000.0 / rate * (double) freq);
            check(chunkStart == origin - freq / 10, "the stalled gap begins 100 ms before the origin");

            juce::int64 written = 28000;   // the worker's stall fill
            juce::int64 credit = 28000;
            bool recorded = false;

            for (const auto frames : backfill)
            {
                const auto early = recorded ? 0 : sm::framesBeforeOrigin(origin, chunkStart, freq, frames, rate);
                chunkStart += (juce::int64) std::llround((double) frames / rate * (double) freq);

                if (early >= frames)
                    continue;

                const auto kept = frames - early;
                const auto skipped = sm::silenceAlreadyWritten(credit, (int) kept, true);
                credit -= skipped;

                if (kept - skipped > 0)
                {
                    written += kept - skipped;
                    recorded = true;
                }
            }

            check(credit == 0, "the whole credit is spent on time after the origin");
            check(written == 31200, "sound after a stalled gap lands at +650 ms");

            // Without the trim the 4800 early frames were counted against the credit, and the
            // same 28000 credit left 8000 of the 36000 to be written: sound at +750 ms.
            check(28000 + (36000 - 28000) == 36000, "without the trim the sound would have landed at +750 ms");
        }

        check(! sm::blockPredatesOrigin(origin, origin - freq / 100, freq, 960, rate),
              "a chunk that straddles the origin does not predate it");
        check(sm::blockPredatesOrigin(origin, origin - freq / 100, freq, 480, rate),
              "a chunk that ends on the origin predates it");
        check(! sm::blockPredatesOrigin(origin, origin, freq, 480, rate), "a chunk that begins on the origin does not");

        // A gap that began after the origin keeps its silence, in front of the first chunk.
        {
            const juce::int64 lateGap = 9600 + 7200;   // last packet ended +50 ms, sound back at +400 ms
            const auto begins = resume - (juce::int64) std::llround((double) lateGap / rate * (double) freq);
            const auto a = sm::alignFirstBlockAt(origin, begins, freq, 9600, rate);
            check(a.dropFrames == 0 && a.silenceFrames == 2400, "a gap that began after the origin is led by its own silence");
            check(a.silenceFrames + lateGap == 19200, "and the sound still lands at +400 ms");
        }

        // A block that began exactly on the origin, or one the source could not date, behaves as before.
        check(sm::alignFirstBlockAt(origin, origin, freq, 480, rate).silenceFrames == 0
                  && sm::alignFirstBlockAt(origin, origin, freq, 480, rate).dropFrames == 0,
              "a block that began on the origin needs nothing");
        {
            const auto a = sm::alignFirstBlockAt(origin, sm::blockStartFromArrival(origin + freq / 250, 480, rate, freq),
                                                 freq, 480, rate);
            const auto b = sm::alignFirstBlock(origin, origin + freq / 250, freq, 480, rate);
            check(a.silenceFrames == b.silenceFrames && a.dropFrames == b.dropFrames,
                  "dated from its arrival, a block aligns as it always did");
        }
        check(sm::alignFirstBlockAt(origin, origin, 0, 480, rate).silenceFrames == 0, "a zero counter frequency is a no-op");
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
