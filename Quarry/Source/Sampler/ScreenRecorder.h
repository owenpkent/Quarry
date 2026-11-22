#pragma once

#include <juce_core/juce_core.h>

//
// The picture that goes with a take: one window, or the main screen, to an mp4 whose
// soundtrack is the same audio the Sample page is recording.
//
// Windows only, like the capture underneath it. Nothing from Windows, WinRT, Direct3D or
// Media Foundation is visible here: all of it lives behind the pimpl in ScreenRecorder.cpp,
// so including this header costs nothing and cannot upset the order of anyone's includes.
//
// The method (Windows.Graphics.Capture, a counter-paced constant frame rate, an H.264 and
// AAC sink writer) was proven in tools/screen_spike. What differs is the sound: the spike
// recorded its own, and here it is fed by the Sample page's recorder, so the wav and the mp4
// are the same samples.
//

#if JUCE_WINDOWS

#include <okstudio/WasapiProcessLoopback.h>

#include <atomic>
#include <memory>

namespace quarry::sampler
{

/**
 * Records a picture and its sound to an mp4 on a worker thread.
 *
 * Threading: start(), finish(), discard() and the queries are message thread. loopbackBlock()
 * is the capture thread and only ever converts and hands over through a lock-free FIFO: it
 * never blocks, never allocates and never touches Media Foundation. Everything else, from
 * opening the encoder to writing the last frame, is one worker thread, so a slow disk or a
 * busy GPU is never felt on the capture thread or the message thread.
 *
 * A failure of the picture is reported through failure() and stops the picture only. It never
 * throws into the caller and never reaches the audio take.
 */
class ScreenRecorder : public okstudio::capture::LoopbackSink
{
public:
    /** What to film. A window handle is an HWND as an integer, the form the Sample page
        already carries. */
    struct Target
    {
        juce::uint64 windowHandle = 0;
        bool primaryMonitor = false;
    };

    /** What finish() produced, or why it produced nothing. */
    struct Finished
    {
        bool ok = false;
        juce::String message;
        juce::File file;
        int width = 0;
        int height = 0;
        double fps = 0.0;
        double durationSec = 0.0;
    };

    ScreenRecorder();
    ~ScreenRecorder() override;

    /**
     * Begins the picture, writing to `tempFile`. `audioSampleRate` is the rate the audio
     * recorder is delivering, known only once it has started, so call this after it.
     *
     * Checks only what is cheap (the window exists and is not minimised), starts the worker
     * and returns at once. It does not wait for the first frame: a window that is not
     * changing may not deliver one for a while, and the message thread must not sit there.
     * If nothing arrives within two seconds the worker sets failure() and gives up.
     */
    juce::Result start(const Target& target, const juce::File& tempFile, double audioSampleRate);

    bool isRecording() const noexcept { return recording.load(); }

    /** Empty while healthy. Set when the picture could not start or died mid-take, and meant
        to be polled by the page timer like SampleRecorder::streamFailure(). */
    juce::String failure() const;

    /**
     * Stops accepting audio and asks the worker to stop ticking, without waiting for it. This
     * is the moment the picture's sound ends: call it the instant Stop is pressed, before
     * anything slow, and finish() afterwards to collect the file. Also ends the wait for the
     * first frame. Message thread; does nothing when no picture is being recorded.
     */
    void requestStop();

    /**
     * Stops ticking, stops accepting audio, writes out what the audio has already delivered,
     * pads the sound with silence to where the picture ends, finalises the mp4 and joins the
     * worker. Safe to call when start() failed or never ran. When ok is false the temp file
     * has been deleted. Calls requestStop() itself, so it may be used alone.
     */
    Finished finish();

    /** Stops and deletes the temp file. The destructor does this too. */
    void discard();

    /** Capture thread. Hands a copy of the block to the worker, or drops it and counts. */
    void loopbackBlock(const float* const* channels, int numChannels, int numSamples) override;

    /** Blocks dropped because the worker fell two seconds behind, for the take's own record. */
    juce::int64 droppedAudioFrames() const noexcept { return droppedFrames.load(); }

private:
    struct Impl;

    void raise(const juce::String& reason);
    void stopWorker();

    std::unique_ptr<Impl> impl;

    std::atomic<bool> recording { false };
    std::atomic<juce::int64> droppedFrames { 0 };

    juce::CriticalSection failureLock;
    juce::String failureText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScreenRecorder)
};

} // namespace quarry::sampler

#endif // JUCE_WINDOWS
