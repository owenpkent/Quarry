#pragma once

#include "SampleMetadata.h"
#include "SourceIdentity.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

//
// One take: arm a source, record, stop, and files land on disk.
//
// Windows only, like the capture underneath it, so callers guard with JUCE_WINDOWS rather
// than an okstudio- or quarry-specific macro. The Sample page hides itself elsewhere.
//

#if JUCE_WINDOWS

#include <okstudio/WasapiProcessLoopback.h>

#include <atomic>
#include <memory>
#include <vector>

namespace quarry::sampler
{

/**
 * Records one application to a float32 WAV plus a JSON sidecar.
 *
 * Nothing runs until start(). There is no rolling buffer: this is explicit record and stop,
 * so an armed source costs nothing until it is recording.
 *
 * Threading: start(), stop() and discard() are message thread. Audio arrives on the capture
 * thread, which is the only thread that touches the chunk list while recording. stop() joins
 * that thread before reading anything, so the processing afterwards needs no lock.
 */
class SampleRecorder : private okstudio::capture::LoopbackSink
{
public:
    /** What stop() produced, or why it produced nothing. */
    struct Written
    {
        bool ok = false;
        juce::String message;
        juce::File audioFile;
        juce::File sidecarFile;
        SampleMetadata metadata;
    };

    SampleRecorder();
    ~SampleRecorder() override;

    /** Opens `source` and begins recording into memory. `libraryRoot` is where the dated
        folders live; it is created if it is not there.

        `windowHandle` is which of the application's windows this take is of, from
        windowsOfSource(). Zero leaves it to be guessed, which is what it was before there was
        anything to pass. */
    juce::Result start(const okstudio::capture::AudioSession& source, const juce::File& libraryRoot,
                       juce::uint64 windowHandle = 0);

    /**
     * Records the whole playback endpoint instead of one application.
     *
     * For a Windows too old for process loopback, and for the times you actually want
     * everything the machine is mixing. What it costs is the thing the rest of this was
     * built for: the source can only be guessed at, so the loudest session at the moment the
     * take starts is written down as a guess and the sidecar says `isolation: endpoint` so
     * nothing downstream mistakes it for a fact.
     */
    juce::Result startEndpoint(const juce::File& libraryRoot);

    /** Stops, trims, measures, and writes. Safe to call when not recording: it reports that
        rather than throwing. */
    Written stop();

    /** Stops and keeps nothing. */
    void discard();

    bool isRecording() const noexcept { return recording.load(); }

    /** How much has been captured, for a clock on screen. */
    double recordedSeconds() const noexcept;

    /** The loudest sample on each side since the last read. */
    struct Peaks
    {
        float left = 0.0f;
        float right = 0.0f;
    };

    /** Loudest sample per side since the last call, for a meter. Reading resets, so exactly
        one caller may use it.

        A mono source reports the same level on both sides rather than silence on the right,
        because what a meter means by an empty channel is "nothing came in on it", and that
        would be a lie about a source that has only one. */
    Peaks readPeaks() noexcept;

    /** Both sides at once, for callers that only want to know how loud it got. Reads, and so
        resets, exactly as readPeaks() does. */
    float readPeak() noexcept;

    /** Non-empty when the stream died under us mid-take. */
    juce::String streamFailure() const;

private:
    //==========================================================================
    void loopbackBlock(const float* const* channels, int numChannels, int numSamples) override;
    void loopbackFailed() override;

    /** Frames per block of captured audio. Appending never reallocates what is already
        held, so a long take costs an occasional small allocation rather than a copy of
        everything so far, which at a hundred megabytes would drop audio on the floor. */
    static constexpr int chunkFrames = 1 << 15;

    struct Chunk
    {
        std::vector<float> data; // channels * chunkFrames, planar within the chunk
    };

    juce::AudioBuffer<float> flatten() const;
    void reset();

    /** Stops whichever of the two streams is running. */
    void _stopStream();

    okstudio::capture::WasapiProcessLoopback loopback;
    okstudio::capture::WasapiLoopback endpointLoopback;
    bool usingEndpoint = false;

    std::vector<std::unique_ptr<Chunk>> chunks;
    int framesInLastChunk = 0;
    int capturedChannels = 0;
    double capturedRate = 0.0;

    std::atomic<bool> recording { false };
    std::atomic<juce::int64> totalFrames { 0 };
    std::atomic<float> peakSinceRead[2] { { 0.0f }, { 0.0f } };

    juce::CriticalSection failureLock;
    juce::String failure;

    SampleMetadata pending;
    juce::File root;

    // Taken when the take starts and written when it ends. By then the window may show
    // something else entirely, and the sample records what was playing.
    juce::Image sourceImage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleRecorder)
};

} // namespace quarry::sampler

#endif // JUCE_WINDOWS
