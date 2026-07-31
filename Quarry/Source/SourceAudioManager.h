//
// Created by Damien Ronssin on 19.06.23.
//

#ifndef SourceAudioManager_h
#define SourceAudioManager_h

#include <JuceHeader.h>
#include "BasicPitchConstants.h"
#include "Resampler.h"
#include "AudioUtils.h"

class QuarryAudioProcessor;

class SourceAudioManager : public ValueTree::Listener
{
public:
    explicit SourceAudioManager(QuarryAudioProcessor* inProcessor);

    ~SourceAudioManager() override;

    /**
     * PrepareToPlay
     * @param inSampleRate Audio sample rate
     * @param inSamplesPerBlock Number of samples per block
     */
    void prepareToPlay(double inSampleRate, int inSamplesPerBlock);

    /**
     * Function to call in Quarry audio processor. Will handle recording if needed.
     * @param inBuffer Input audio buffer
     */
    void processBlock(const AudioBuffer<float>& inBuffer);

    /**
     * Function to call when start record button is clicked.
     * Will prepare everything needed to record and the recording will start in the next processBlock
     */
    void startRecording();

    /**
     * Start recording from an audio input device Quarry opened itself (see
     * AudioInputManager) rather than from the audio the host sends us. The device runs at its own
     * sample rate and block size, so they are passed in here instead of taken from the host.
     * Blocks arriving through processBlock are ignored until stopRecording().
     * @param inSampleRate Sample rate of the capture device
     * @param inNumChannels Number of channels to record (at most 2)
     * @param inNumSamplesPerBlock Capture device block size
     */
    void startRecordingFromExternalInput(double inSampleRate, int inNumChannels, int inNumSamplesPerBlock);

    /**
     * Function to call with each block from the input device Quarry opened itself.
     * Called on that device's audio thread.
     * @param inBuffer Captured audio buffer
     */
    void processExternalInputBlock(const AudioBuffer<float>& inBuffer);

    /**
     * Function to call when stop record button is clicked.
     * Will stop properly the recording and then launch the transcription.
     */
    void stopRecording();

    /**
     * Function to call when a file is dropped on the audio region to load it.
     * @param inFile Audio file to load
     * @return Whether audio file load was successful
     */
    bool onFileDrop(const File& inFile);

    /**
     * Stop recording if needed and then reset/clear everything owned by this class.
     */
    void clear();

    /**
     * To call only when the recording/file loading is fully completed, otherwise you'll get and empty buffer.
     * @return A reference to the downsampled source audio.
     */
    AudioBuffer<float>& getDownsampledSourceAudioForTranscription();

    /**
     * Get source audio at current processor sample rate.
     * @return Reference to source audio buffer (recorded or loaded from file).
     */
    AudioBuffer<float>& getSourceAudioForPlayback();

    /**
     * Return a string containing the filename of the dropped audio file.
     * If the source audio was recorded (not loaded from file), an empty string is returned.
     * @return Filename of dropped audio file, or empty string if source audio recorded.
     */
    String getDroppedFilename() const;

    /**
     * Get number of samples currently acquired (either recorded or loaded from file) at basic pitch sample rate (22.05 kHz)
     * Note that if recording is ongoing, those sample are not yet available in buffers returned by getDownsampledSourceAudioForTranscription() and getSourceAudioForPlayback().
     * @return Number of source audio samples already recorded or loaded at basic pitch sample rate.
     */
    int getNumSamplesDownAcquired() const;

    /**
     * Same as getNumSamplesDownAcquired() but in seconds instead of number of samples.
     * @return The duration in seconds of the audio acquired for transcription.
     */
    double getAudioSampleDuration() const;

    /**
     * @return Pointer to source audio thumbnail
     */
    AudioThumbnail* getAudioThumbnail();

private:
    void valueTreePropertyChanged(ValueTree& treeWhosePropertyHasChanged, const Identifier& property) override;

    void _startRecording(double inSampleRate, int inNumChannels, int inNumSamplesPerBlock);

    /** Write one block to the recording. Called on whichever audio thread is feeding us. */
    void _writeBlock(const AudioBuffer<float>& inBuffer);

    void _deleteFilesToDelete();

    QuarryAudioProcessor* mProcessor;

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> mThreadedWriter;
    juce::TimeSliceThread mWriterThread = juce::TimeSliceThread("Source Audio Writer Thread");

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> mThreadedWriterDown;
    juce::TimeSliceThread mWriterThreadDown = juce::TimeSliceThread("Downsampled Source Audio Writer Thread");
    CriticalSection mWriterLock;

    Resampler mDownSampler = {};

    const int mSourceSamplesPerThumbnailSample = 128;
    juce::AudioFormatManager mThumbnailFormatManager;
    juce::AudioThumbnailCache mThumbnailCache;
    juce::AudioThumbnail mThumbnail;

    const File mQuarryDir =
        File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getChildFile("Quarry");
    File mSourceFile;
    File mRecordedFileDown;

    AudioBuffer<float> mSourceAudio;
    AudioBuffer<float> mDownsampledSourceAudio; // Always at basic pitch sample rate

    // Sample rate for mSourceAudio buffer
    double mSourceAudioSampleRate = 44100;

    std::vector<juce::File> mFilesToDelete;

    double mSampleRate = 44100;
    int mSamplesPerBlock = 512;

    // Sample rate and channel count the current recording is being written at. Same as the host's
    // unless the recording is coming from an input device Quarry opened itself.
    double mRecordSampleRate = 44100;
    int mRecordNumChannels = 1;

    std::atomic<bool> mIsExternalInputRecording = false;

    unsigned long long mNumSamplesAcquired = 0;
    unsigned long long mNumSamplesAcquiredDown = 0;
    double mDuration = 0.0;

    String mDroppedFilename;

    AudioBuffer<float> mInternalMonoBuffer;
    AudioBuffer<float> mInternalDownsampledBuffer;

    std::atomic<bool> mIsRecording = false;
};

#endif // SourceAudioManager_h
