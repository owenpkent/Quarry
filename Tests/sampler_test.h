//
// Checking the arithmetic a captured sample rests on: where it gets cropped, how loud it is
// and what it ends up called.
//

#ifndef QUARRY_SAMPLER_TEST_H
#define QUARRY_SAMPLER_TEST_H

#include <cmath>
#include <iostream>
#include <vector>

#include "Sampler/SampleLibrary.h"
#include "Sampler/SampleMath.h"
#include "Sampler/SampleMetadata.h"

namespace sampler_test_utils
{
using namespace quarry::sampler;

static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (! condition)
    {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}

static void checkNear(double actual, double expected, double tolerance, const std::string& what)
{
    if (std::fabs(actual - expected) > tolerance)
    {
        std::cout << "  FAILED: " << what << " (got " << actual << ", wanted " << expected << " +/- "
                  << tolerance << ")" << std::endl;
        ++failures;
    }
}

/** A sine of the given amplitude on every channel, with optional silence either side. */
static std::vector<std::vector<float>> makeTone(int numChannels, int numSamples, double sampleRate,
                                                double frequency, double amplitude,
                                                int leadingSilence = 0, int trailingSilence = 0)
{
    std::vector<std::vector<float>> channels((size_t) numChannels,
                                             std::vector<float>((size_t) numSamples, 0.0f));

    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = leadingSilence; i < numSamples - trailingSilence; ++i)
            channels[(size_t) ch][(size_t) i] =
                (float) (amplitude * std::sin(2.0 * juce::MathConstants<double>::pi * frequency
                                              * (double) i / sampleRate));

    return channels;
}

static std::vector<const float*> pointersTo(const std::vector<std::vector<float>>& channels)
{
    std::vector<const float*> pointers;

    for (const auto& channel : channels)
        pointers.push_back(channel.data());

    return pointers;
}
} // namespace sampler_test_utils

inline bool sampler_test()
{
    using namespace sampler_test_utils;
    failures = 0;

    constexpr double sampleRate = 48000.0;

    //==========================================================================
    // Trimming.
    {
        const auto tone = makeTone(2, 48000, sampleRate, 440.0, 0.5, 12000, 12000);
        const auto pointers = pointersTo(tone);
        const auto bounds = findTrimBounds(pointers.data(), 2, 48000);

        check(bounds.foundSound, "trim finds the sound");
        checkNear(bounds.startSample, 12000, 50, "trim starts where the tone does");
        checkNear(bounds.endSample, 36000, 50, "trim ends where the tone does");
    }
    {
        // Silence throughout must report the full range, not a zero-length sample: an empty
        // crop downstream looks like a bug, and this is the one case that is not one.
        std::vector<std::vector<float>> silence(2, std::vector<float>(1000, 0.0f));
        const auto pointers = pointersTo(silence);
        const auto bounds = findTrimBounds(pointers.data(), 2, 1000);

        check(! bounds.foundSound, "silence reports no sound found");
        check(bounds.startSample == 0 && bounds.endSample == 1000, "silence keeps the whole range");
    }
    {
        // Padding gives back a little of what was cut, and never runs off either end.
        const auto tone = makeTone(1, 10000, sampleRate, 440.0, 0.5, 5000, 0);
        const auto pointers = pointersTo(tone);
        const auto bounds = findTrimBounds(pointers.data(), 1, 10000, -60.0f, 1000);

        checkNear(bounds.startSample, 4000, 50, "padding pulls the start back");
        check(bounds.endSample == 10000, "padding cannot run past the end");
    }

    //==========================================================================
    // Loudness. A 1 kHz sine at -20 dBFS on both channels is the calibration case the
    // -0.691 offset in BS.1770 exists for: it should read about -20 LUFS. If the
    // K-weighting were wrong, or read from a 48 kHz table at another rate, this is what
    // would catch it.
    {
        const auto tone = makeTone(2, (int) (sampleRate * 3), sampleRate, 1000.0, 0.1);
        const auto pointers = pointersTo(tone);
        const auto loudness = measureLoudness(pointers.data(), 2, (int) (sampleRate * 3), sampleRate);

        checkNear(loudness.peakDb, -20.0, 0.2, "peak of a -20 dBFS sine");
        checkNear(loudness.lufs, -20.0, 1.0, "integrated loudness of a -20 dBFS 1 kHz sine");
        check(loudness.truePeakDb >= loudness.peakDb, "true peak is never below sample peak");
    }
    {
        // The same tone at 44.1 kHz must read the same. This is the whole reason the filter
        // is computed from the sample rate.
        const auto tone = makeTone(2, (int) (44100.0 * 3), 44100.0, 1000.0, 0.1);
        const auto pointers = pointersTo(tone);
        const auto loudness = measureLoudness(pointers.data(), 2, (int) (44100.0 * 3), 44100.0);

        checkNear(loudness.lufs, -20.0, 1.0, "integrated loudness is the same at 44.1 kHz");
    }
    {
        std::vector<std::vector<float>> silence(2, std::vector<float>(48000, 0.0f));
        const auto pointers = pointersTo(silence);
        const auto loudness = measureLoudness(pointers.data(), 2, 48000, sampleRate);

        check(loudness.peakDb <= -143.0f, "silence reports the floor, not negative infinity");
        check(loudness.lufs <= -143.0f, "silent loudness reports the floor");
    }
    {
        // Shorter than one 400 ms gating block: there is no integrated value to give, and
        // inventing one would be worse than declining.
        const auto tone = makeTone(1, 4800, sampleRate, 1000.0, 0.5);
        const auto pointers = pointersTo(tone);
        const auto loudness = measureLoudness(pointers.data(), 1, 4800, sampleRate);

        check(loudness.peakDb > -10.0f, "a short capture still reports its peak");
        check(loudness.lufs <= -143.0f, "a capture shorter than a gating block reports no loudness");
    }

    //==========================================================================
    // Names.
    check(slugify("Never Gonna Give You Up") == "never-gonna-give-you-up", "slug of a plain title");
    check(slugify("  Hello,   World!  ") == "hello-world", "punctuation and runs collapse to one hyphen");
    check(slugify("AC/DC - Back In Black") == "ac-dc-back-in-black", "slashes separate rather than vanish");
    check(slugify("").isEmpty(), "an empty title slugs to nothing");
    check(slugify("!!!").isEmpty(), "a title of only punctuation slugs to nothing");
    check(! slugify("a").isEmpty(), "a one character title survives");

    {
        // Capping happens on a word boundary, so a name reads as shortened rather than
        // corrupted, and never ends on a stray hyphen.
        const auto capped = slugify("the quick brown fox jumps over the lazy dog", 20);
        check(capped.length() <= 20, "a long title is capped");
        check(! capped.endsWithChar('-'), "a capped title never ends on a hyphen");
        check(capped.startsWith("the-quick-brown"), "capping keeps the front of the title");
    }

    {
        // Straight off a real capture: the count is Chrome's unread notifications and the
        // last segment names the browser we already recorded the name of.
        check(tidyWindowTitle("(413) silence - YouTube - Google Chrome", "chrome.exe") == "silence - YouTube",
              "a browser title loses its notification count and its own name");
        check(tidyWindowTitle("silence - YouTube", "chrome.exe") == "silence - YouTube",
              "a title that names no browser is left alone");
        check(tidyWindowTitle("(4) Inbox", "chrome.exe") == "Inbox", "a bare count still goes");
        check(tidyWindowTitle("Half-Life 2", "hl2.exe") == "Half-Life 2",
              "a hyphen inside a name is not a separator to peel at");
        check(tidyWindowTitle("Some Track - Spotify", "Spotify.exe") == "Some Track",
              "the rule is general, not a list of browsers");
        check(tidyWindowTitle("(x) Not A Count", "chrome.exe") == "(x) Not A Count",
              "only digits count as a count");
    }

    {
        const auto when = juce::Time(2026, 7, 16, 14, 30, 52); // month is zero based: August
        check(dateFolder(when) == "2026-08/2026-08-16", "date folders are year-month then full date");

        const auto stem = sampleStem(when, "chrome.exe", "Never Gonna Give You Up - YouTube");
        check(stem.startsWith("143052-chrome-"), "the stem is time then source");
        check(stem.contains("never-gonna-give-you-up"), "the stem carries the title");
        check(! stem.contains("exe"), "the stem drops the exe extension");

        check(sampleStem(when, "chrome.exe", "") == "143052-chrome",
              "a capture with no title is still a sensible name");
        check(sampleStem(when, "", "") == "143052", "a capture with nothing known is still named");
    }

    {
        // Nothing a capture writes ever replaces something already on disk.
        const juce::StringArray taken { "143052-chrome", "143052-chrome-02" };
        const auto isTaken = [&taken](const juce::String& s) { return taken.contains(s); };

        check(uniqueStem("143052-chrome", isTaken) == "143052-chrome-03", "collisions count past what is taken");
        check(uniqueStem("143052-firefox", isTaken) == "143052-firefox", "a free name is left alone");
    }

    //==========================================================================
    // The sidecar path guard. A security boundary, not a convenience: the name comes from a
    // file that may have arrived from another machine, and what it names is later passed to
    // moveToTrash. These are the shapes a crafted sidecar in a downloaded sample pack would
    // use to reach outside its own folder.
    {
        using quarry::sampler::SampleLibrary;

        const auto sidecar = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                 .getChildFile("captures")
                                 .getChildFile("take.json");

        check(SampleLibrary::siblingNamed(sidecar, "take.png")
                  == sidecar.getSiblingFile("take.png"),
              "a plain name beside the sidecar resolves");

        check(SampleLibrary::siblingNamed(sidecar, "../escape.png") == juce::File(),
              "a relative escape is refused");
        check(SampleLibrary::siblingNamed(sidecar, "../../../../windows/system32/x.dll") == juce::File(),
              "a deep relative escape is refused");
        check(SampleLibrary::siblingNamed(sidecar, "sub/take.png") == juce::File(),
              "a forward slash is refused");
        check(SampleLibrary::siblingNamed(sidecar, "sub\\take.png") == juce::File(),
              "a backslash is refused");
        check(SampleLibrary::siblingNamed(sidecar, "C:\\windows\\x.dll") == juce::File(),
              "an absolute path is refused");
        check(SampleLibrary::siblingNamed(sidecar, "") == juce::File(), "an empty name is refused");

        check(SampleLibrary::siblingNamed(sidecar, "take.mp4") == sidecar.getSiblingFile("take.mp4"),
              "a video beside the sidecar resolves");
        check(SampleLibrary::siblingNamed(sidecar, "..\\x.mp4") == juce::File(),
              "a video name that climbs out is refused");
    }

    //==========================================================================
    // The picture, read back through scan() the way the page does.
    {
        using quarry::sampler::SampleLibrary;

        const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("quarry-sampler-video-test");
        dir.deleteRecursively();
        dir.createDirectory();

        dir.getChildFile("a.wav").replaceWithText("x");
        dir.getChildFile("a.mp4").replaceWithText("x");
        dir.getChildFile("a.json").replaceWithText(
            "{\"schema\":1,\"capturedAt\":\"2026-08-16T14:30:52.000Z\",\"video\":{\"file\":\"a.mp4\","
            "\"width\":1280,\"height\":720,\"fps\":60,\"durationSec\":2.5}}");
        dir.getChildFile("b.wav").replaceWithText("x");
        dir.getChildFile("b.json").replaceWithText(
            "{\"schema\":1,\"capturedAt\":\"2026-08-16T14:30:53.000Z\",\"video\":{\"file\":\"..\\\\x.mp4\"}}");
        dir.getChildFile("c.wav").replaceWithText("x");
        dir.getChildFile("c.json").replaceWithText("{\"schema\":1,\"capturedAt\":\"2026-08-16T14:30:54.000Z\"}");

        auto entries = SampleLibrary::scan(dir);
        check(entries.size() == 3, "all three sidecars are read");

        // A sidecar that names a video that is not there reads as having none.
        dir.getChildFile("a.mp4").deleteFile();
        {
            const auto gone = SampleLibrary::scan(dir);
            auto sawA = false;

            for (const auto& entry : gone)
                if (entry.sidecarFile.getFileNameWithoutExtension() == "a")
                {
                    sawA = true;
                    check(entry.videoFile == juce::File(), "a named video that is missing is not kept");
                }

            check(sawA, "the take with the missing video is still listed");
        }

        dir.getChildFile("a.mp4").replaceWithText("x");
        entries = SampleLibrary::scan(dir);

        for (const auto& entry : entries)
        {
            const auto name = entry.sidecarFile.getFileNameWithoutExtension();

            if (name == "a")
                check(entry.videoFile == dir.getChildFile("a.mp4"), "a sidecar naming a video resolves it");
            else
                check(entry.videoFile == juce::File(), "a traversal name or no video leaves it unset");
        }

        dir.deleteRecursively();
    }

    //==========================================================================
    // The sidecar's video block: there only when there is a picture, and additive.
    {
        quarry::sampler::SampleMetadata meta;
        check(! meta.toJson().contains("\"video\""), "a take without a picture writes no video block");

        meta.video.file = "take.mp4";
        meta.video.width = 1280;
        meta.video.height = 720;
        meta.video.fps = 60.0;
        meta.video.durationSec = 2.5;

        const auto parsed = juce::JSON::parse(meta.toJson());
        const auto video = parsed.getProperty("video", juce::var());
        check(video.isObject(), "a take with a picture writes a video block");
        check(video.getProperty("file", juce::var()).toString() == "take.mp4", "the video block names the file");
        check((int) video.getProperty("width", juce::var()) == 1280, "the video block carries the width");
        check((int) parsed.getProperty("schema", juce::var()) == 1, "the schema number is unchanged");
    }

    if (failures == 0)
        std::cout << "  all sampler checks passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_SAMPLER_TEST_H
