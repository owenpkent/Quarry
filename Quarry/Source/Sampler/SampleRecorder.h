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
        folders live; it is created if it is not there. */
    juce::Result start(const okstudio::capture::AudioSession& source, const juce::File& libraryRoot);

    /** Stops, trims, measures, and writes. Safe to call when not recording: it reports that
        rather than throwing. */
    Written stop();

    /** Stops and keeps nothing. */
    void discard();

    bool isRecording() const noexcept { return recording.load(); }

    /** How much has been captured, for a clock on screen. */
    double recordedSeconds() const noexcept;

    /** Loudest sample since the last call, for a meter. Reading it resets it, so exactly one
        caller may use it. */
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

    okstudio::capture::WasapiProcessLoopback loopback;

    std::vector<std::unique_ptr<Chunk>> chunks;
    int framesInLastChunk = 0;
    int capturedChannels = 0;
    double capturedRate = 0.0;

    std::atomic<bool> recording { false };
    std::atomic<juce::int64> totalFrames { 0 };
    std::atomic<float> peakSinceRead { 0.0f };

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
