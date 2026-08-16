//
// Recording one application to a float32 WAV plus its sidecar.
//

// Before SampleRecorder.h, deliberately: that header reaches the Windows audio stack, and
// windows.h defines a Rectangle() that makes juce::Rectangle ambiguous in anything parsed
// after it. Same reasoning as AudioInputManager.cpp.
#include <juce_audio_formats/juce_audio_formats.h>

#include "SampleRecorder.h"

#if JUCE_WINDOWS

#include <cstring>
#include <limits>

namespace quarry::sampler
{

//==============================================================================
SampleRecorder::SampleRecorder() = default;

SampleRecorder::~SampleRecorder()
{
    discard();
}

void SampleRecorder::reset()
{
    chunks.clear();
    framesInLastChunk = 0;
    capturedChannels = 0;
    capturedRate = 0.0;
    totalFrames.store(0);
    peakSinceRead.store(0.0f);
    sourceImage = juce::Image();

    const juce::ScopedLock lock(failureLock);
    failure.clear();
}

juce::Result SampleRecorder::start(const okstudio::capture::AudioSession& source, const juce::File& libraryRoot)
{
    if (recording.load())
        return juce::Result::fail("Already recording");

    reset();
    root = libraryRoot;

    pending = SampleMetadata();
    pending.capturedAt = juce::Time::getCurrentTime();
    pending.source.isolatedToProcess = true;
    pending.source.processId = source.processId;
    pending.source.processName = source.processName;
    pending.source.processPath = source.executablePath;
    pending.source.sessionVolume = source.volume;

    // Asked once, at the start. The window may be showing something else by the time this
    // take ends, and what a sample records is what was playing.
    const auto identity = identifySource(source.processId);
    pending.source.windowTitle = identity.windowTitle;
    pending.source.url = identity.url;

    sourceImage = captureWindowImage(source.processId);

    usingEndpoint = false;

    // Armed before the stream opens, not after: the capture thread starts delivering the
    // moment Start() returns, and a flag set afterwards would throw the first blocks away.
    recording.store(true);

    const auto opened = loopback.start(source.processId, *this);

    if (opened.failed())
    {
        recording.store(false);
        return opened;
    }

    capturedRate = loopback.sampleRate();
    return juce::Result::ok();
}

juce::Result SampleRecorder::startEndpoint(const juce::File& libraryRoot)
{
    if (recording.load())
        return juce::Result::fail("Already recording");

    reset();
    root = libraryRoot;

    pending = SampleMetadata();
    pending.capturedAt = juce::Time::getCurrentTime();
    pending.source.isolatedToProcess = false;
    pending.source.processName = "System audio";

    // The source is a guess in this mode, so it is made once, out loud, and marked as one:
    // the loudest thing playing when the take starts. Everything written about it below is
    // qualified by isolation being "endpoint" in the sidecar.
    const okstudio::capture::AudioSession* loudest = nullptr;
    const auto sessions = okstudio::capture::WasapiProcessLoopback::sessions();

    for (const auto& session : sessions)
        if (loudest == nullptr || session.peak > loudest->peak)
            loudest = &session;

    if (loudest != nullptr && loudest->peak > 0.0001f)
    {
        pending.source.processId = loudest->processId;
        pending.source.processName = loudest->processName;
        pending.source.processPath = loudest->executablePath;
        pending.source.sessionVolume = loudest->volume;

        const auto identity = identifySource(loudest->processId);
        pending.source.windowTitle = identity.windowTitle;
        pending.source.url = identity.url;

        sourceImage = captureWindowImage(loudest->processId);
    }

    usingEndpoint = true;
    recording.store(true);

    // An empty endpoint id means the default playback device, and a channel cap of 2 keeps a
    // 5.1 output from becoming a six channel sample nobody asked for.
    const auto opened = endpointLoopback.start({}, *this, 2);

    if (opened.failed())
    {
        recording.store(false);
        usingEndpoint = false;
        return opened;
    }

    capturedRate = endpointLoopback.sampleRate();
    return juce::Result::ok();
}

void SampleRecorder::_stopStream()
{
    if (usingEndpoint)
        endpointLoopback.stop();
    else
        loopback.stop();
}

void SampleRecorder::discard()
{
    recording.store(false);
    _stopStream();
    reset();
}

//==============================================================================
void SampleRecorder::loopbackBlock(const float* const* channels, int numChannels, int numSamples)
{
    if (! recording.load() || channels == nullptr || numChannels <= 0 || numSamples <= 0)
        return;

    // Taken from the callback rather than from the stream, so the first block is never
    // dropped waiting for the message thread to write down what the format turned out to be.
    if (capturedChannels == 0)
        capturedChannels = numChannels;

    const auto usable = juce::jmin(numChannels, capturedChannels);

    float blockPeak = 0.0f;
    for (int ch = 0; ch < usable; ++ch)
        for (int i = 0; i < numSamples; ++i)
            blockPeak = juce::jmax(blockPeak, std::fabs(channels[ch][i]));

    auto previous = peakSinceRead.load();
    while (blockPeak > previous && ! peakSinceRead.compare_exchange_weak(previous, blockPeak))
    {
    }

    int done = 0;

    while (done < numSamples)
    {
        if (chunks.empty() || framesInLastChunk >= chunkFrames)
        {
            auto chunk = std::make_unique<Chunk>();
            chunk->data.assign((size_t) capturedChannels * (size_t) chunkFrames, 0.0f);
            chunks.push_back(std::move(chunk));
            framesInLastChunk = 0;
        }

        const auto room = chunkFrames - framesInLastChunk;
        const auto taking = juce::jmin(room, numSamples - done);
        auto& chunk = *chunks.back();

        for (int ch = 0; ch < usable; ++ch)
            std::memcpy(chunk.data.data() + (size_t) ch * (size_t) chunkFrames + (size_t) framesInLastChunk,
                        channels[ch] + done,
                        (size_t) taking * sizeof(float));

        framesInLastChunk += taking;
        done += taking;
        totalFrames.fetch_add(taking);
    }
}

void SampleRecorder::loopbackFailed()
{
    const juce::ScopedLock lock(failureLock);
    failure = "The audio stream stopped: the application may have closed.";
}

//==============================================================================
double SampleRecorder::recordedSeconds() const noexcept
{
    return capturedRate > 0.0 ? (double) totalFrames.load() / capturedRate : 0.0;
}

float SampleRecorder::readPeak() noexcept
{
    return peakSinceRead.exchange(0.0f);
}

juce::String SampleRecorder::streamFailure() const
{
    const juce::ScopedLock lock(failureLock);
    return failure;
}

juce::AudioBuffer<float> SampleRecorder::flatten() const
{
    const auto captured = totalFrames.load();

    // Parenthesised because windows.h, which the capture header drags in, defines max as a
    // macro and would otherwise eat this call.
    const auto frameLimit = (juce::int64) (std::numeric_limits<int>::max)();
    const auto frames = (int) juce::jlimit((juce::int64) 0, frameLimit, captured);

    juce::AudioBuffer<float> buffer(juce::jmax(1, capturedChannels), juce::jmax(1, frames));
    buffer.clear();

    int written = 0;

    for (size_t index = 0; index < chunks.size() && written < frames; ++index)
    {
        const auto isLast = index + 1 == chunks.size();
        const auto available = isLast ? framesInLastChunk : chunkFrames;
        const auto taking = juce::jmin(available, frames - written);

        for (int ch = 0; ch < capturedChannels; ++ch)
            buffer.copyFrom(ch, written,
                            chunks[index]->data.data() + (size_t) ch * (size_t) chunkFrames,
                            taking);

        written += taking;
    }

    return buffer;
}

//==============================================================================
SampleRecorder::Written SampleRecorder::stop()
{
    Written result;

    if (! recording.load())
    {
        result.message = "Not recording";
        return result;
    }

    recording.store(false);

    // Joins the capture thread, so everything below runs with the chunk list to itself.
    _stopStream();

    const auto rate = capturedRate > 0.0
                        ? capturedRate
                        : (usingEndpoint ? endpointLoopback.sampleRate() : loopback.sampleRate());

    if (totalFrames.load() <= 0 || capturedChannels <= 0 || rate <= 0.0)
    {
        result.message = "Nothing was captured. The application may not have played anything.";
        reset();
        return result;
    }

    auto captured = flatten();
    const auto originalFrames = captured.getNumSamples();

    // Trim to where the sound is. A capture with no sound keeps its whole range rather than
    // collapsing to nothing, and says so, because a zero length file downstream reads as a
    // bug rather than as an empty take.
    const auto padding = (int) (rate * 0.002);
    const auto bounds = findTrimBounds(captured.getArrayOfReadPointers(),
                                       captured.getNumChannels(),
                                       originalFrames,
                                       -60.0f,
                                       padding);

    juce::AudioBuffer<float> trimmed(captured.getNumChannels(), juce::jmax(1, bounds.length()));
    for (int ch = 0; ch < captured.getNumChannels(); ++ch)
        trimmed.copyFrom(ch, 0, captured, ch, bounds.startSample, bounds.length());

    const auto loudness = measureLoudness(trimmed.getArrayOfReadPointers(),
                                          trimmed.getNumChannels(),
                                          trimmed.getNumSamples(),
                                          rate);

    pending.audio.sampleRate  = rate;
    pending.audio.channels    = trimmed.getNumChannels();
    pending.audio.durationSec = (double) trimmed.getNumSamples() / rate;
    pending.audio.loudness    = loudness;

    pending.trim.foundSound          = bounds.foundSound;
    pending.trim.startSec            = (double) bounds.startSample / rate;
    pending.trim.endSec              = (double) bounds.endSample / rate;
    pending.trim.originalDurationSec = (double) originalFrames / rate;

    //==========================================================================
    // Where it goes.
    const auto folder = root.getChildFile(dateFolder(pending.capturedAt));

    if (! folder.createDirectory())
    {
        result.message = "Could not create " + folder.getFullPathName();
        reset();
        return result;
    }

    const auto titleForName = pending.media.known && pending.media.title.isNotEmpty()
                                ? pending.media.title
                                : pending.source.windowTitle;

    const auto stem = uniqueStem(sampleStem(pending.capturedAt, pending.source.processName, titleForName),
                                 [&folder](const juce::String& candidate)
                                 {
                                     return folder.getChildFile(candidate + ".wav").existsAsFile();
                                 });

    auto audioFile   = folder.getChildFile(stem + ".wav");
    auto sidecarFile = folder.getChildFile(stem + ".json");

    // The picture, if there is one. Written before the sidecar so the sidecar only ever
    // names a file that is already there.
    if (sourceImage.isValid())
    {
        auto imageFile = folder.getChildFile(stem + ".png");
        juce::FileOutputStream imageStream(imageFile);

        if (imageStream.openedOk())
        {
            juce::PNGImageFormat png;

            if (png.writeImageToStream(sourceImage, imageStream))
                pending.source.screenshotFile = imageFile.getFileName();
            else
                imageFile.deleteFile();
        }
    }

    //==========================================================================
    // The audio. Float32 straight through: what loopback handed us is what lands on disk,
    // with no conversion, no dither and no decision about peaks over 0 dBFS.
    juce::StringPairArray tags;
    tags.set(juce::WavAudioFormat::bwavDescription, pending.description());
    tags.set(juce::WavAudioFormat::bwavOriginator, "Quarry");
    tags.set(juce::WavAudioFormat::bwavOriginationDate, pending.capturedAt.formatted("%Y-%m-%d"));
    tags.set(juce::WavAudioFormat::bwavOriginationTime, pending.capturedAt.formatted("%H:%M:%S"));
    tags.set(juce::WavAudioFormat::riffInfoSoftware, "Quarry");
    tags.set(juce::WavAudioFormat::riffInfoSource, pending.source.processName);

    if (pending.source.windowTitle.isNotEmpty())
        tags.set(juce::WavAudioFormat::riffInfoTitle, pending.source.windowTitle);

    if (pending.media.known && pending.media.artist.isNotEmpty())
        tags.set(juce::WavAudioFormat::riffInfoArtist, pending.media.artist);

    auto stream = std::make_unique<juce::FileOutputStream>(audioFile);

    if (! stream->openedOk())
    {
        result.message = "Could not write " + audioFile.getFullPathName();
        reset();
        return result;
    }

    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(stream.get(), rate, (unsigned int) trimmed.getNumChannels(), 32, tags, 0));

        if (writer == nullptr)
        {
            result.message = "Could not write a wav for this capture";
            reset();
            return result;
        }

        // The writer owns the stream from here, and must be destroyed before the file is
        // read back, which is what this scope is for.
        stream.release();
        writer->writeFromAudioSampleBuffer(trimmed, 0, trimmed.getNumSamples());
    }

    //==========================================================================
    // The sidecar, which is the complete record and the thing a browser reads.
    if (! sidecarFile.replaceWithText(pending.toJson()))
    {
        result.message = "Wrote the audio but could not write " + sidecarFile.getFileName();
        result.audioFile = audioFile;
        reset();
        return result;
    }

    result.ok = true;
    result.audioFile = audioFile;
    result.sidecarFile = sidecarFile;
    result.metadata = pending;
    result.message = bounds.foundSound
                       ? juce::String("Kept ") + juce::String(pending.audio.durationSec, 2) + " s"
                       : "Kept a take with no sound in it";

    reset();
    return result;
}

} // namespace quarry::sampler

#endif // JUCE_WINDOWS
