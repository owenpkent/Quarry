//
// Created by Damien Ronssin on 19.06.23.
//

#include <memory>

#include "SourceAudioManager.h"
#include "PluginProcessor.h"

SourceAudioManager::SourceAudioManager(QuarryAudioProcessor* inProcessor)
    : mProcessor(inProcessor)
    , mThumbnailCache(1)
    , mThumbnail(mSourceSamplesPerThumbnailSample, mThumbnailFormatManager, mThumbnailCache)
{
    mProcessor->addListenerToStateValueTree(this);
    jassert(mProcessor->getValueTree().hasProperty(NnId::SourceAudioNativeSrPathId));
}

SourceAudioManager::~SourceAudioManager()
{
    mProcessor->removeListenerFromStateValueTree(this);
}

void SourceAudioManager::prepareToPlay(double inSampleRate, int inSamplesPerBlock)
{
    if (mIsRecording.load()) {
        stopRecording();
    }

    mSampleRate = inSampleRate;
    mSamplesPerBlock = inSamplesPerBlock;
    mInternalMonoBuffer.setSize(1, inSamplesPerBlock);
    mDownSampler.prepareToPlay(inSampleRate, inSamplesPerBlock, BASIC_PITCH_SAMPLE_RATE);
    mInternalDownsampledBuffer.setSize(
        1, static_cast<int>(std::ceil(BASIC_PITCH_SAMPLE_RATE / inSampleRate * inSamplesPerBlock)) + 5);

    auto state = mProcessor->getState();
    if ((state == PopulatedAudioAndMidiRegions || state == Processing) && mSampleRate != mSourceAudioSampleRate) {
        AudioBuffer<float> tmp_buffer;
        AudioUtils::resampleBuffer(mSourceAudio, tmp_buffer, mSourceAudioSampleRate, mSampleRate);
        mSourceAudio = std::move(tmp_buffer);
        mSourceAudioSampleRate = mSampleRate;
    }
}

void SourceAudioManager::processBlock(const AudioBuffer<float>& inBuffer)
{
    // While recording from an input device of our own, the host's audio is not the source.
    if (mIsExternalInputRecording.load())
        return;

    _writeBlock(inBuffer);
}

void SourceAudioManager::processExternalInputBlock(const AudioBuffer<float>& inBuffer)
{
    if (!mIsExternalInputRecording.load())
        return;

    _writeBlock(inBuffer);
}

void SourceAudioManager::_writeBlock(const AudioBuffer<float>& inBuffer)
{
    if (!mIsRecording.load())
        return;

    ScopedLock sl(mWriterLock);

    // Recording may have been stopped between the check above and taking the lock.
    if (!mIsRecording.load() || mThreadedWriter == nullptr || mThreadedWriterDown == nullptr)
        return;

    // Write incoming audio to file at native sample rate
    bool result = mThreadedWriter->write(inBuffer.getArrayOfReadPointers(), inBuffer.getNumSamples());
    jassertquiet(result);
    mNumSamplesAcquired += inBuffer.getNumSamples();
    mDuration = static_cast<double>(mNumSamplesAcquiredDown) / BASIC_PITCH_SAMPLE_RATE;

    // Downmix to mono
    mInternalMonoBuffer.copyFrom(0, 0, inBuffer, 0, 0, inBuffer.getNumSamples());
    for (int ch = 1; ch < inBuffer.getNumChannels(); ch++) {
        mInternalMonoBuffer.addFrom(0, 0, inBuffer, ch, 0, inBuffer.getNumSamples());
    }

    mInternalMonoBuffer.applyGain(1.0f / static_cast<float>(inBuffer.getNumChannels()));

    // Downsample to basic pitch sample rate
    int num_samples_down = mDownSampler.processBlock(mInternalMonoBuffer.getReadPointer(0),
                                                     mInternalDownsampledBuffer.getWritePointer(0),
                                                     inBuffer.getNumSamples());
    jassert(num_samples_down <= mInternalDownsampledBuffer.getNumSamples());

    // Write downsampled audio to file at downsampled sample rate
    bool result_down =
        mThreadedWriterDown->write(mInternalDownsampledBuffer.getArrayOfReadPointers(), num_samples_down);
    jassertquiet(result_down);

    mNumSamplesAcquiredDown += num_samples_down;
}

void SourceAudioManager::startRecording()
{
    mIsExternalInputRecording.store(false);
    _startRecording(mSampleRate, std::min(mProcessor->getTotalNumInputChannels(), 2), mSamplesPerBlock);
}

void SourceAudioManager::startRecordingFromExternalInput(double inSampleRate,
                                                         int inNumChannels,
                                                         int inNumSamplesPerBlock)
{
    mIsExternalInputRecording.store(true);
    _startRecording(inSampleRate, std::min(inNumChannels, 2), inNumSamplesPerBlock);
}

void SourceAudioManager::_startRecording(double inSampleRate, int inNumChannels, int inNumSamplesPerBlock)
{
    // If already recording (should not happen but if it does, simply return)
    if (mIsRecording.load()) {
        jassertfalse;
        return;
    }

    mRecordSampleRate = inSampleRate;
    mRecordNumChannels = std::max(inNumChannels, 1);

    // The capture device may not run at the host's sample rate or block size, so size the internal
    // buffers and the downsampler for whatever is actually about to be recorded. Safe to allocate
    // here: nothing is writing to them until mIsRecording goes true at the end of this function.
    mInternalMonoBuffer.setSize(1, inNumSamplesPerBlock, false, false, true);
    mDownSampler.prepareToPlay(mRecordSampleRate, inNumSamplesPerBlock, BASIC_PITCH_SAMPLE_RATE);
    mInternalDownsampledBuffer.setSize(
        1,
        static_cast<int>(std::ceil(BASIC_PITCH_SAMPLE_RATE / mRecordSampleRate * inNumSamplesPerBlock)) + 5,
        false,
        false,
        true);

    // Prepare files to be written
    if (!mQuarryDir.isDirectory()) {
        bool res = mQuarryDir.createDirectory();
        jassertquiet(res);
    }

    mDroppedFilename = "";

    String timestamp = Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S");

    mSourceFile = mQuarryDir.getChildFile("recorded_audio" + timestamp + ".wav");
    mRecordedFileDown = mQuarryDir.getChildFile("recorded_audio" + timestamp + "_downsampled.wav");

    size_t i = 1;

    while (mSourceFile.existsAsFile() || mRecordedFileDown.existsAsFile()) {
        mSourceFile = mQuarryDir.getChildFile("recorded_audio" + timestamp + "_" + String(i) + ".wav");
        mRecordedFileDown =
            mQuarryDir.getChildFile("recorded_audio" + timestamp + "_" + String(i) + "_downsampled.wav");
        i += 1;
    }

    Result file_creation_result = mSourceFile.create();
    Result file_creation_result_down = mRecordedFileDown.create();

    if (!file_creation_result.wasOk() || !file_creation_result_down.wasOk()) {
        mProcessor->clear();
        NativeMessageBox::showMessageBoxAsync(
            MessageBoxIconType::NoIcon, "Error", "File creation for recording failed.");
        return;
    }

    mFilesToDelete.push_back(mSourceFile);
    mFilesToDelete.push_back(mRecordedFileDown);

    // Init first writer at native sample rate (stereo)
    WavAudioFormat format;
    StringPairArray meta_data_values;

    auto* wav_writer = format.createWriterFor(
        new FileOutputStream(mSourceFile), mRecordSampleRate, mRecordNumChannels, 16, meta_data_values, 0);

    mWriterThread.startThread();
    mThreadedWriter = std::make_unique<AudioFormatWriter::ThreadedWriter>(wav_writer, mWriterThread, 32768);

    // Init second writer at basic pitch sample rate (mono)
    WavAudioFormat format_down;
    StringPairArray meta_data_values_down;

    auto* wav_writer_down = format_down.createWriterFor(
        new FileOutputStream(mRecordedFileDown), BASIC_PITCH_SAMPLE_RATE, 1, 16, meta_data_values_down, 0);

    mWriterThreadDown.startThread();
    mThreadedWriterDown =
        std::make_unique<AudioFormatWriter::ThreadedWriter>(wav_writer_down, mWriterThreadDown, 32768);
    mDownSampler.reset();

    mThreadedWriterDown->setDataReceiver(&mThumbnail);

    mNumSamplesAcquired = 0;
    mNumSamplesAcquiredDown = 0;

    mIsRecording.store(true);
    mProcessor->setStateToRecording();
}

void SourceAudioManager::stopRecording()
{
    {
        ScopedLock sl(mWriterLock);
        mIsRecording.store(false);
        mThreadedWriter.reset();
        mThreadedWriterDown.reset();
    }

    mIsExternalInputRecording.store(false);

    mWriterThread.stopThread(1000);
    mWriterThreadDown.stopThread(1000);

    bool success = AudioUtils::loadAudioFile(mSourceFile, mSourceAudio, mSourceAudioSampleRate);

    // Should def not happen
    if (!success) {
        mProcessor->clear();
        NativeMessageBox::showMessageBoxAsync(
            MessageBoxIconType::NoIcon, "Could not load the recorded audio sample.", "");
        return;
    }

    double dummy_sr;
    success = AudioUtils::loadAudioFile(mRecordedFileDown, mDownsampledSourceAudio, dummy_sr);
    jassert(dummy_sr == BASIC_PITCH_SAMPLE_RATE);

    // Should def not happen
    if (!success) {
        mProcessor->clear();
        NativeMessageBox::showMessageBoxAsync(
            MessageBoxIconType::NoIcon, "Could not load the recorded audio sample.", "");
        return;
    }

    // A device we opened ourselves records at its own rate, which needn't be the host's.
    if (mSourceAudioSampleRate != mSampleRate) {
        AudioBuffer<float> tmp_buffer;
        AudioUtils::resampleBuffer(mSourceAudio, tmp_buffer, mSourceAudioSampleRate, mSampleRate);
        mSourceAudio = std::move(tmp_buffer);
        mSourceAudioSampleRate = mSampleRate;
        mNumSamplesAcquired = static_cast<unsigned long long>(mSourceAudio.getNumSamples());
    }

    auto& tree = mProcessor->getValueTree();
    tree.setPropertyExcludingListener(this, NnId::SourceAudioNativeSrPathId, mSourceFile.getFullPathName(), nullptr);

    mProcessor->getTranscriptionManager()->setLaunchNewTranscription();
}

bool SourceAudioManager::onFileDrop(const File& inFile)
{
    if (mProcessor->getState() == EmptyAudioAndMidiRegions || mProcessor->getState() == PopulatedAudioAndMidiRegions) {
        mProcessor->clear();
        bool success = AudioUtils::loadAudioFile(inFile, mSourceAudio, mSourceAudioSampleRate);

        if (!success) {
            mProcessor->clear();
            NativeMessageBox::showMessageBoxAsync(
                MessageBoxIconType::NoIcon,
                "Could not load the audio file.",
                "Check your file format (Accepted formats: .wav, .aiff, .flac, .mp3, .ogg).");
            return false;
        }

        // Downsample to basic pitch sample rate
        AudioUtils::resampleBuffer(
            mSourceAudio, mDownsampledSourceAudio, mSourceAudioSampleRate, BASIC_PITCH_SAMPLE_RATE);

        // Resample to current plugin sample rate for playback
        if (mSourceAudioSampleRate != mSampleRate) {
            AudioBuffer<float> tmp_buffer;
            AudioUtils::resampleBuffer(mSourceAudio, tmp_buffer, mSourceAudioSampleRate, mSampleRate);
            mSourceAudio = std::move(tmp_buffer);
            mSourceAudioSampleRate = mSampleRate;
        }

        mNumSamplesAcquiredDown = mDownsampledSourceAudio.getNumSamples();
        mNumSamplesAcquired = mSourceAudio.getNumSamples();
        mDuration = static_cast<double>(mNumSamplesAcquiredDown) / BASIC_PITCH_SAMPLE_RATE;

        mDroppedFilename = inFile.getFileNameWithoutExtension();
        mSourceFile = inFile;

        auto& tree = mProcessor->getValueTree();
        tree.setPropertyExcludingListener(this, NnId::SourceAudioNativeSrPathId, inFile.getFullPathName(), nullptr);
        mProcessor->getTranscriptionManager()->getTimeQuantizeOptions().fileLoaded();

        mThumbnail.clear();
        mThumbnailCache.clear();
        mThumbnail.setSource(&mDownsampledSourceAudio, BASIC_PITCH_SAMPLE_RATE, 0);

        // Launch transcription job
        mProcessor->getTranscriptionManager()->launchTranscribeJob();

    } else {
        jassertfalse;
    }

    return true;
}

void SourceAudioManager::clear()
{
    if (mIsRecording) {
        stopRecording();
    }

    mSourceAudio = {};
    mDownsampledSourceAudio = {};

    mNumSamplesAcquiredDown = 0;
    mNumSamplesAcquired = 0;
    mDuration = 0.0;

    _deleteFilesToDelete();

    mProcessor->getValueTree().setPropertyExcludingListener(this, NnId::SourceAudioNativeSrPathId, String(), nullptr);

    mDroppedFilename = "";
}

AudioBuffer<float>& SourceAudioManager::getDownsampledSourceAudioForTranscription()
{
    return mDownsampledSourceAudio;
}

AudioBuffer<float>& SourceAudioManager::getSourceAudioForPlayback()
{
    return mSourceAudio;
}

String SourceAudioManager::getDroppedFilename() const
{
    return mDroppedFilename;
}

int SourceAudioManager::getNumSamplesDownAcquired() const
{
    return static_cast<int>(mNumSamplesAcquiredDown);
}

double SourceAudioManager::getAudioSampleDuration() const
{
    return mDuration;
}

AudioThumbnail* SourceAudioManager::getAudioThumbnail()
{
    return &mThumbnail;
}

void SourceAudioManager::valueTreePropertyChanged(ValueTree& treeWhosePropertyHasChanged, const Identifier& property)
{
    if (property == NnId::SourceAudioNativeSrPathId) {
        auto path = treeWhosePropertyHasChanged.getProperty(property).toString();

        if (path != mSourceFile.getFullPathName()) {
            if (path.isEmpty()) {
                clear();
                return;
            }

            onFileDrop(File(path));

            // If loading state from recorded audio, add to files to delete on clear.
            if (mSourceFile.getParentDirectory() == mQuarryDir) {
                mRecordedFileDown =
                    mQuarryDir.getChildFile(mSourceFile.getFileNameWithoutExtension() + "_downsampled.wav");
                mFilesToDelete.push_back(mSourceFile);
                mFilesToDelete.push_back(mRecordedFileDown);
                mDroppedFilename = "";
            }
        }
    }
}

void SourceAudioManager::_deleteFilesToDelete()
{
    for (auto& file: mFilesToDelete) {
        // Make sure we only ever delete files that have been recorded, not loaded from disk.
        if (file.getParentDirectory() == mQuarryDir) {
            bool res = file.deleteFile();
            jassertquiet(res);
        } else {
            jassertfalse;
        }
    }

    mFilesToDelete.clear();
}
