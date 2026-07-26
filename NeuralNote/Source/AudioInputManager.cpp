//
// Audio input device selection and direct recording for NeuralNoteVideo.
//

#include "AudioInputManager.h"
#include "PluginProcessor.h"

namespace
{
const String kDriverKey = "inputDriver";
const String kDeviceKey = "inputDevice";
const String kChannelSetKey = "inputChannelSet";
const String kConfiguredKey = "inputConfigured";
} // namespace

AudioInputManager::AudioInputManager(NeuralNoteAudioProcessor* inProcessor)
    : mProcessor(inProcessor)
{
}

AudioInputManager::~AudioInputManager()
{
    mIsRecording.store(false);
    _closeInputDevice();
}

const String& AudioInputManager::getHostInputName()
{
    static const String name = "Host input (no device)";
    return name;
}

StringArray AudioInputManager::getDriverNames()
{
    StringArray names;

    for (auto* type: mDeviceManager.getAvailableDeviceTypes())
        names.add(type->getTypeName());

    return names;
}

void AudioInputManager::setSelectedDriverName(const String& inDriverName)
{
    if (inDriverName == mSelectedDriverName)
        return;

    // The inputs of the previous driver mean nothing to the new one.
    setSelectedInputDeviceName({});

    mSelectedDriverName = inDriverName;
    _saveSelection();
}

StringArray AudioInputManager::getInputDeviceNames()
{
    if (auto* type = _getSelectedDriver()) {
        type->scanForDevices();
        return type->getDeviceNames(true);
    }

    return {};
}

void AudioInputManager::setSelectedInputDeviceName(const String& inDeviceName)
{
    if (inDeviceName == mSelectedInputDeviceName)
        return;

    jassert(!mIsRecording.load()); // The UI locks the pickers while recording.

    _closeInputDevice();

    mSelectedInputDeviceName = inDeviceName;
    mSelectedChannelSet = 0;
    mLastError.clear();
    _saveSelection();

    if (mPanelOnScreen && mSelectedInputDeviceName.isNotEmpty())
        _openInputDevice();
}

StringArray AudioInputManager::getInputChannelSetNames()
{
    StringArray names;

    auto* device = mDeviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return names;

    auto channel_names = device->getInputChannelNames();
    const int num_channels = channel_names.size();

    for (int ch = 0; ch + 1 < num_channels; ch += 2)
        names.add(String(ch + 1) + " + " + String(ch + 2) + " (stereo)");

    for (int ch = 0; ch < num_channels; ch++)
        names.add(String(ch + 1) + " (mono)");

    return names;
}

void AudioInputManager::setSelectedInputChannelSet(int inIndex)
{
    if (inIndex == mSelectedChannelSet || inIndex < 0)
        return;

    mSelectedChannelSet = inIndex;
    _saveSelection();

    // Reopen so the device runs with only the requested channels enabled.
    if (isInputDeviceOpen() && !mIsRecording.load()) {
        _closeInputDevice();
        _openInputDevice();
    }
}

bool AudioInputManager::isInputDeviceOpen() const
{
    auto* device = mDeviceManager.getCurrentAudioDevice();
    return device != nullptr && device->isOpen();
}

double AudioInputManager::getCurrentSampleRate() const
{
    if (auto* device = mDeviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 0.0;
}

void AudioInputManager::setPanelOnScreen(bool inIsOnScreen)
{
    if (inIsOnScreen == mPanelOnScreen)
        return;

    mPanelOnScreen = inIsOnScreen;

    if (mPanelOnScreen) {
        _ensureInitialised();

        if (mSelectedInputDeviceName.isNotEmpty())
            _openInputDevice();
    } else if (!mIsRecording.load()) {
        // Don't keep a microphone open behind the user's back.
        _closeInputDevice();
    }
}

float AudioInputManager::getAndResetPeakLevel()
{
    return mPeakLevel.exchange(0.0f);
}

bool AudioInputManager::startRecording()
{
    if (mIsRecording.load()) {
        jassertfalse;
        return false;
    }

    if (mSelectedInputDeviceName.isEmpty())
        return false;

    if (!_openInputDevice())
        return false;

    auto* device = mDeviceManager.getCurrentAudioDevice();
    const auto active_inputs = device->getActiveInputChannels().countNumberOfSetBits();

    if (active_inputs <= 0) {
        mLastError = "No input channels are enabled on this device.";
        return false;
    }

    mNumRecordedChannels = jmin(active_inputs, 2);

    mProcessor->getSourceAudioManager()->startRecordingFromExternalInput(
        device->getCurrentSampleRate(), mNumRecordedChannels, device->getCurrentBufferSizeSamples());

    mIsRecording.store(true);
    return true;
}

void AudioInputManager::stopRecording()
{
    if (!mIsRecording.load())
        return;

    mIsRecording.store(false);
    mProcessor->getSourceAudioManager()->stopRecording();

    if (!mPanelOnScreen)
        _closeInputDevice();
}

void AudioInputManager::abortRecording()
{
    mIsRecording.store(false);

    if (!mPanelOnScreen)
        _closeInputDevice();
}

void AudioInputManager::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                         int numInputChannels,
                                                         float* const* outputChannelData,
                                                         int numOutputChannels,
                                                         int numSamples,
                                                         const AudioIODeviceCallbackContext&)
{
    // We asked for an input-only device, but never hand a host back uninitialised output.
    for (int ch = 0; ch < numOutputChannels; ch++)
        if (outputChannelData[ch] != nullptr)
            FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    if (numSamples <= 0)
        return;

    float* channels[kMaxCaptureChannels];
    int num_channels = 0;

    for (int ch = 0; ch < numInputChannels && num_channels < kMaxCaptureChannels; ch++)
        if (inputChannelData[ch] != nullptr)
            channels[num_channels++] = const_cast<float*>(inputChannelData[ch]);

    if (num_channels == 0)
        return;

    AudioBuffer<float> buffer(channels, num_channels, numSamples);

    float peak = 0.0f;

    for (int ch = 0; ch < num_channels; ch++)
        peak = jmax(peak, buffer.getMagnitude(ch, 0, numSamples));

    float previous_peak = mPeakLevel.load();

    while (peak > previous_peak && !mPeakLevel.compare_exchange_weak(previous_peak, peak)) {
    }

    if (mIsRecording.load()) {
        // The writers were made for the channel count we started with, no more.
        AudioBuffer<float> to_record(channels, jmin(num_channels, mNumRecordedChannels), numSamples);
        mProcessor->getSourceAudioManager()->processExternalInputBlock(to_record);
    }
}

void AudioInputManager::audioDeviceAboutToStart(AudioIODevice*)
{
    mPeakLevel.store(0.0f);
}

void AudioInputManager::audioDeviceStopped()
{
    mPeakLevel.store(0.0f);
}

void AudioInputManager::audioDeviceError(const String& errorMessage)
{
    mLastError = errorMessage;
}

void AudioInputManager::_ensureInitialised()
{
    if (mInitialised)
        return;

    PropertiesFile::Options options;
    options.applicationName = "NeuralNoteVideoAudioInput";
    options.filenameSuffix = "settings";
    options.folderName = "NeuralNote";
    options.osxLibrarySubFolder = "Application Support";
    mProperties = std::make_unique<PropertiesFile>(options);

    const bool was_configured_before = mProperties->getBoolValue(kConfiguredKey, false);

    _loadSelection();
    mInitialised = true;

    if (mSelectedDriverName.isEmpty()) {
        auto drivers = getDriverNames();

        if (!drivers.isEmpty())
            mSelectedDriverName = mDeviceManager.getCurrentAudioDeviceType().isNotEmpty()
                                      ? mDeviceManager.getCurrentAudioDeviceType()
                                      : drivers[0];
    }

    // The standalone app mutes the audio input it is given, to avoid a feedback loop, so "host
    // input" would record silence there. Someone running the app on its own wants to record from
    // this computer anyway, so start them on its default input. Only ever done once: if they then
    // choose the host input, that choice stands.
    if (!was_configured_before && mProcessor->wrapperType == AudioProcessor::wrapperType_Standalone) {
        if (auto* type = _getSelectedDriver()) {
            type->scanForDevices();

            const auto device_names = type->getDeviceNames(true);
            const int default_index = type->getDefaultDeviceIndex(true);

            if (isPositiveAndBelow(default_index, device_names.size()))
                mSelectedInputDeviceName = device_names[default_index];
        }
    }

    mProperties->setValue(kConfiguredKey, true);
    _saveSelection();
}

AudioIODeviceType* AudioInputManager::_getSelectedDriver()
{
    _ensureInitialised();

    for (auto* type: mDeviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == mSelectedDriverName)
            return type;

    return nullptr;
}

bool AudioInputManager::_openInputDevice()
{
    _ensureInitialised();

    if (mSelectedInputDeviceName.isEmpty())
        return false;

    if (isInputDeviceOpen() && mDeviceManager.getAudioDeviceSetup().inputDeviceName == mSelectedInputDeviceName)
        return true;

    mLastError.clear();

    if (!mDeviceManagerInitialised) {
        // Two input channels wanted, no outputs. This is the only call that can briefly touch a
        // device other than the chosen one, so name ours as the preferred default.
        mDeviceManager.initialise(2, 0, nullptr, false, mSelectedInputDeviceName);
        mDeviceManagerInitialised = true;
    }

    if (mDeviceManager.getCurrentAudioDeviceType() != mSelectedDriverName)
        mDeviceManager.setCurrentAudioDeviceType(mSelectedDriverName, true);

    auto setup = mDeviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = mSelectedInputDeviceName;
    setup.outputDeviceName = {};
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();

    mLastError = mDeviceManager.setAudioDeviceSetup(setup, true);

    if (mLastError.isNotEmpty() || !isInputDeviceOpen()) {
        if (mLastError.isEmpty())
            mLastError = "Could not open " + mSelectedInputDeviceName + ".";

        return false;
    }

    // Now the device is open we know how many channels it has, so the channel choice can be applied.
    if (auto* device = mDeviceManager.getCurrentAudioDevice()) {
        const auto mask = _getInputChannelMask(device->getInputChannelNames().size());

        if (mask.countNumberOfSetBits() > 0 && mask != device->getActiveInputChannels()) {
            setup = mDeviceManager.getAudioDeviceSetup();
            setup.useDefaultInputChannels = false;
            setup.inputChannels = mask;

            const auto channel_error = mDeviceManager.setAudioDeviceSetup(setup, true);

            if (channel_error.isNotEmpty())
                mLastError = channel_error;
        }
    }

    if (!isInputDeviceOpen())
        return false;

    mDeviceManager.addAudioCallback(this);
    return true;
}

void AudioInputManager::_closeInputDevice()
{
    mDeviceManager.removeAudioCallback(this);
    mDeviceManager.closeAudioDevice();
    mPeakLevel.store(0.0f);
}

BigInteger AudioInputManager::_getInputChannelMask(int inNumDeviceInputChannels) const
{
    BigInteger mask;

    if (inNumDeviceInputChannels <= 0)
        return mask;

    const int num_pairs = inNumDeviceInputChannels / 2;

    if (mSelectedChannelSet < num_pairs) {
        mask.setBit(mSelectedChannelSet * 2);
        mask.setBit(mSelectedChannelSet * 2 + 1);
    } else {
        const int mono_channel = mSelectedChannelSet - num_pairs;

        if (mono_channel >= 0 && mono_channel < inNumDeviceInputChannels)
            mask.setBit(mono_channel);
        else
            mask.setRange(0, jmin(2, inNumDeviceInputChannels), true);
    }

    return mask;
}

void AudioInputManager::_saveSelection() const
{
    if (mProperties == nullptr)
        return;

    mProperties->setValue(kDriverKey, mSelectedDriverName);
    mProperties->setValue(kDeviceKey, mSelectedInputDeviceName);
    mProperties->setValue(kChannelSetKey, mSelectedChannelSet);
    mProperties->saveIfNeeded();
}

void AudioInputManager::_loadSelection()
{
    if (mProperties == nullptr)
        return;

    mSelectedDriverName = mProperties->getValue(kDriverKey, {});
    mSelectedInputDeviceName = mProperties->getValue(kDeviceKey, {});
    mSelectedChannelSet = mProperties->getIntValue(kChannelSetKey, 0);
}
