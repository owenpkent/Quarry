//
// Compile-and-link smoke test for the sampler's recording path, plus a manual harness.
//
// The unit test runner cannot touch SampleRecorder: it reaches juce_audio_formats and the
// Windows audio stack, and the pure arithmetic it rests on is already covered by
// sampler_test.h. Without this target nothing would compile the recorder except the plugin,
// and nothing at all would ever run it outside the UI.
//
// CI only builds it. Run it by hand against a machine that is making a sound:
//   SamplerSmoke                       list what is playing, and exit
//   SamplerSmoke <pid> <seconds> [dir] record that application and write the files
//   SamplerSmoke all  <seconds> [dir]  record the whole endpoint instead, the fallback path
//   SamplerSmoke library [query] [dir] read the library back, the way the browser does
//
// There is deliberately no add_test(): a runner with no audio would record its own silence
// and prove nothing. Same reasoning as the kit's okstudio_audio_capture_smoke.
//

#include <juce_audio_formats/juce_audio_formats.h>

#include "Sampler/SampleLibrary.h"
#include "Sampler/SampleRecorder.h"

#include <cstdio>
#include <cstdlib>

#if JUCE_WINDOWS

using okstudio::capture::WasapiProcessLoopback;
using quarry::sampler::SampleRecorder;

namespace
{
int listSources()
{
    if (! WasapiProcessLoopback::isSupported())
    {
        std::printf("This Windows is older than build 20348: no per-application recording.\n");
        return 1;
    }

    const auto sessions = WasapiProcessLoopback::sessions();

    if (sessions.empty())
    {
        std::printf("Nothing has an audio session on the default playback device.\n");
        return 0;
    }

    std::printf("%-8s %-28s %-8s %-8s %s\n", "PID", "APPLICATION", "VOLUME", "PEAK", "STATE");

    for (const auto& session : sessions)
        std::printf("%-8u %-28s %5.0f%%   %-8.4f %s%s\n",
                    (unsigned) session.processId,
                    session.processName.toRawUTF8(),
                    session.volume * 100.0f,
                    session.peak,
                    session.isPlaying ? "playing" : "idle",
                    session.volume < 0.999f ? "   <-- below 100%, baked into any capture" : "");

    std::printf("\nPick a pid with a moving peak and pass it in.\n");
    return 0;
}

/** Reads the library back off its own sidecars, which is what the browser does at startup. */
int showLibrary(const juce::File& root, const juce::String& query)
{
    const auto all = quarry::sampler::SampleLibrary::scan(root);
    const auto shown = quarry::sampler::SampleLibrary::filter(all, query);

    std::printf("root          : %s\n", root.getFullPathName().toRawUTF8());
    std::printf("captured      : %d\n", (int) all.size());

    if (query.isNotEmpty())
        std::printf("matching '%s' : %d\n", query.toRawUTF8(), (int) shown.size());

    std::printf("\n%-52s %-7s %-9s %s\n", "NAME", "LENGTH", "LOUDNESS", "SOURCE");

    for (const auto& entry : shown)
        std::printf("%-52s %5.1fs  %6.1f LUFS %s%s\n",
                    entry.displayName().toRawUTF8(),
                    entry.durationSec,
                    entry.lufs,
                    entry.appName.toRawUTF8(),
                    entry.isolatedToProcess ? "" : " (guessed)");

    if (! shown.empty())
    {
        const auto& first = shown.front();
        std::printf("\nnewest        : %s\n", first.displayName().toRawUTF8());
        std::printf("  title       : %s\n", first.windowTitle.toRawUTF8());
        std::printf("  url         : %s\n", first.url.isEmpty() ? "(none)" : first.url.toRawUTF8());
        std::printf("  screenshot  : %s\n", first.imageFile.existsAsFile() ? "present" : "(none)");
    }

    return all.empty() ? 1 : 0;
}

int record(juce::uint32 processId, double seconds, const juce::File& root, bool everything)
{
    okstudio::capture::AudioSession chosen;
    bool found = false;

    if (! everything)
    {
        for (const auto& session : WasapiProcessLoopback::sessions())
        {
            if (session.processId == processId)
            {
                chosen = session;
                found = true;
                break;
            }
        }

        if (! found)
        {
            std::printf("No audio session for pid %u. Run with no arguments to see what there is.\n",
                        (unsigned) processId);
            return 1;
        }
    }

    if (everything)
        std::printf("recording     : everything this computer plays (source will be a guess)\n");
    else
        std::printf("recording     : %s (pid %u)\n", chosen.processName.toRawUTF8(), (unsigned) processId);

    if (! everything)
        std::printf("session volume: %.0f%%%s\n", chosen.volume * 100.0f,
                    chosen.volume < 0.999f ? "  <-- this loss is permanent" : "");

    std::printf("writing to    : %s\n\n", root.getFullPathName().toRawUTF8());

    SampleRecorder recorder;
    const auto started = everything ? recorder.startEndpoint(root) : recorder.start(chosen, root);

    if (started.failed())
    {
        std::printf("FAILED to start: %s\n", started.getErrorMessage().toRawUTF8());
        return 1;
    }

    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) (seconds * 1000.0);

    while (juce::Time::getMillisecondCounter() < until)
    {
        juce::Thread::sleep(200);
        std::printf("\r  %.1f s, peak %.4f   ", recorder.recordedSeconds(), recorder.readPeak());
        std::fflush(stdout);
    }

    std::printf("\n\n");

    const auto written = recorder.stop();

    if (! written.ok)
    {
        std::printf("FAILED: %s\n", written.message.toRawUTF8());
        return 1;
    }

    const auto& meta = written.metadata;

    std::printf("--- kept ---\n");
    std::printf("isolation     : %s\n", meta.source.isolatedToProcess ? "process" : "endpoint (source guessed)");
    std::printf("audio         : %s (%lld bytes)\n",
                written.audioFile.getFullPathName().toRawUTF8(),
                (long long) written.audioFile.getSize());
    std::printf("sidecar       : %s\n", written.sidecarFile.getFileName().toRawUTF8());
    std::printf("window title  : %s\n",
                meta.source.windowTitle.isEmpty() ? "(none found)" : meta.source.windowTitle.toRawUTF8());
    std::printf("url           : %s\n",
                meta.source.url.isEmpty() ? "(none, or not a browser)" : meta.source.url.toRawUTF8());
    std::printf("screenshot    : %s\n",
                meta.source.screenshotFile.isEmpty() ? "(none)" : meta.source.screenshotFile.toRawUTF8());
    std::printf("format        : %d ch @ %.0f Hz float32\n", meta.audio.channels, meta.audio.sampleRate);
    std::printf("duration      : %.2f s kept of %.2f s captured\n",
                meta.audio.durationSec, meta.trim.originalDurationSec);
    std::printf("trimmed       : %s (%.3f s to %.3f s)\n",
                meta.trim.foundSound ? "yes" : "no sound found",
                meta.trim.startSec, meta.trim.endSec);
    std::printf("peak          : %.2f dBFS\n", meta.audio.loudness.peakDb);
    std::printf("true peak     : %.2f dBFS\n", meta.audio.loudness.truePeakDb);
    std::printf("loudness      : %.2f LUFS\n", meta.audio.loudness.lufs);

    // Read the file back rather than trusting that writing it worked. A wav that does not
    // open is exactly the failure this whole design was arranged to avoid.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(written.audioFile));

    if (reader == nullptr)
    {
        std::printf("\nFAILED: the wav we just wrote does not open.\n");
        return 1;
    }

    std::printf("\nreads back    : %d ch @ %.0f Hz, %d bit%s, %lld frames\n",
                (int) reader->numChannels, reader->sampleRate, reader->bitsPerSample,
                reader->usesFloatingPointData ? " float" : " int",
                (long long) reader->lengthInSamples);

    const auto roundTripped = reader->usesFloatingPointData && reader->bitsPerSample == 32;
    std::printf("float32       : %s\n", roundTripped ? "yes" : "NO, something converted it");

    return roundTripped ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    // No JUCE initialiser: nothing here touches the message thread. The recorder's capture
    // thread is its own, and the file writing is plain juce_core.
    if (argc < 2)
        return listSources();

    const auto everything = juce::String(argv[1]) == "all";
    const auto processId = everything ? 0u : (juce::uint32) std::strtoul(argv[1], nullptr, 10);
    const auto seconds = argc > 2 ? std::atof(argv[2]) : 8.0;

    const auto root = argc > 3
                        ? juce::File(juce::String(juce::CharPointer_UTF8(argv[3])))
                        : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                              .getChildFile("Quarry Captures");

    if (juce::String(argv[1]) == "library")
        return showLibrary(argc > 3 ? juce::File(juce::String(juce::CharPointer_UTF8(argv[3]))) : root,
                           argc > 2 ? juce::String(juce::CharPointer_UTF8(argv[2])) : juce::String());

    return record(processId, seconds, root, everything);
}

#else

int main()
{
    std::printf("Per-application capture is a Windows feature. Nothing to smoke here.\n");
    return 0;
}

#endif
