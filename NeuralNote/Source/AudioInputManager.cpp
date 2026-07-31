//
// Audio input device selection and direct recording for NeuralNoteVideo.
//

#include "AudioInputManager.h"
#include "PluginProcessor.h"

// Last, deliberately: this drags in the Windows audio headers, and windows.h defines a Rectangle()
// that makes juce::Rectangle ambiguous in anything parsed after it.
#if JUCE_WINDOWS
    #include <okstudio/WasapiLoopback.h>
#endif

namespace
{
const String kDriverKey = "inputDriver";
const String kDeviceKey = "inputDevice";
const String kChannelSetKey = "inputChannelSet";
const String kConfiguredKey = "inputConfigured";
const String kSystemAudioDefaultKey = "inputSystemAudioDefaultApplied";

#if JUCE_WINDOWS
// Same string as okstudio::capture::loopbackTypeName. Spelled out here rather than including
// AudioCapture.h, which brings its own capture source model along with it.
const String kSystemAudioDriverName = "System Audio";

// A loopback endpoint is stereo as far as we care, and the recorder only ever keeps two.
constexpr int kLoopbackChannelCap = 2;

// A loopback packet has no fixed length, so pick the block the writers are sized for and cut
// whatever arrives down to it.
constexpr int kLoopbackBlockSizeSamples = 2048;
#endif
} // namespace

#if JUCE_WINDOWS
/**
 * The WASAPI loopback stream and its sink, in one place. Blocks arriving on the capture thread go
 * straight to AudioInputManager::_processInputBlock(), which is where an audio device's input
 * blocks go too, so both are metered and recorded by the same code.
 */
struct AudioInputManager::LoopbackCapture : private okstudio::capture::LoopbackSink
{
    explicit LoopbackCapture(AudioInputManager& inOwner)
        : mOwner(inOwner)
    {
    }

    // Stopped here, in the derived destructor's body: the capture thread calls back through this
    // object's vtable, and by the time the base class is destroyed there is nothing to call.
    ~LoopbackCapture() override { mStream.stop(); }

    Result start(const String& inEndpointId, int inChannelCap)
    {
        return mStream.start(inEndpointId, *this, inChannelCap);
    }

    /** Joins the capture thread, so no block can arrive once this has returned. */
    void stop() { mStream.stop(); }

    double sampleRate() const noexcept { return mStream.sampleRate(); }

    int channelCount() const noexcept { return mStream.channelCount(); }

    bool hasFailed() const noexcept { return mStream.hasFailed(); }

private:
    void loopbackBlock(const float* const* channels, int numChannels, int numSamples) override
    {
        mOwner._processInputBlock(channels, numChannels, numSamples);
    }

    void loopbackFailed() override
    {
        // Capture thread: setting a flag is all that is safe from here.
        mOwner.mLoopbackFailed.store(true);
    }

    AudioInputManager& mOwner;
    okstudio::capture::WasapiLoopback mStream;
};
#endif

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

#if JUCE_WINDOWS
    // First, because recording what the computer is playing is what this app is for.
    names.add(kSystemAudioDriverName);
#endif

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
#if JUCE_WINDOWS
    if (_isLoopbackDriverSelected()) {
        _ensureInitialised();
        _scanLoopbackEndpoints();
        return mLoopbackEndpointNames;
    }
#endif

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

#if JUCE_WINDOWS
    if (_isLoopbackDriverSelected()) {
        // A playback endpoint is recorded as the stereo pair it is, so there is one honest entry
        // rather than an empty combo box.
        names.add("1 + 2 (stereo)");
        return names;
    }
#endif

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
#if JUCE_WINDOWS
    if (mLoopback != nullptr)
        return !mLoopback->hasFailed();
#endif

    auto* device = mDeviceManager.getCurrentAudioDevice();
    return device != nullptr && device->isOpen();
}

double AudioInputManager::getCurrentSampleRate() const
{
#if JUCE_WINDOWS
    if (mLoopback != nullptr)
        return mLoopback->sampleRate();
#endif

    if (auto* device = mDeviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 0.0;
}

String AudioInputManager::getLastError()
{
#if JUCE_WINDOWS
    // The stream can die long after it was opened (endpoint unplugged, format changed), and the
    // capture thread can only set a flag, so turn that into a message here.
    if (mLoopbackFailed.exchange(false))
        mLastError = "The system audio output stopped. Choose it again to reconnect.";
#endif

    return mLastError;
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

    _ensureInitialised();

    if (mSelectedInputDeviceName.isEmpty())
        return false;

#if JUCE_WINDOWS
    if (_isLoopbackDriverSelected()) {
        // Always a fresh stream, even if one is already open for the level meter. WASAPI reports a
        // device position that keeps running while the endpoint is silent, and the kit pads any gap
        // in it with silence, so a stream opened when the panel appeared would prepend every quiet
        // second before Record to the take. Starting it here is what resets that bookkeeping.
        _closeLoopbackDevice();

        if (!_openLoopbackDevice())
            return false;

        const auto num_channels = mLoopback->channelCount();

        if (num_channels <= 0) {
            mLastError = "That output has no channels to record.";
            return false;
        }

        mNumRecordedChannels = jmin(num_channels, kLoopbackChannelCap);
        mNumRecordedSamplesPerBlock = kLoopbackBlockSizeSamples;

        mProcessor->getSourceAudioManager()->startRecordingFromExternalInput(
            mLoopback->sampleRate(), mNumRecordedChannels, mNumRecordedSamplesPerBlock);

        mIsRecording.store(true);
        return true;
    }
#endif

    if (!_openInputDevice())
        return false;

    auto* device = mDeviceManager.getCurrentAudioDevice();
    const auto active_inputs = device->getActiveInputChannels().countNumberOfSetBits();

    if (active_inputs <= 0) {
        mLastError = "No input channels are enabled on this device.";
        return false;
    }

    mNumRecordedChannels = jmin(active_inputs, 2);
    mNumRecordedSamplesPerBlock = jmax(1, device->getCurrentBufferSizeSamples());

    mProcessor->getSourceAudioManager()->startRecordingFromExternalInput(
        device->getCurrentSampleRate(), mNumRecordedChannels, mNumRecordedSamplesPerBlock);

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

    _processInputBlock(inputChannelData, numInputChannels, numSamples);
}

void AudioInputManager::_processInputBlock(const float* const* inChannelData, int inNumChannels, int inNumSamples)
{
    if (inNumSamples <= 0)
        return;

    float* channels[kMaxCaptureChannels];
    int num_channels = 0;

    for (int ch = 0; ch < inNumChannels && num_channels < kMaxCaptureChannels; ch++)
        if (inChannelData[ch] != nullptr)
            channels[num_channels++] = const_cast<float*>(inChannelData[ch]);

    if (num_channels == 0)
        return;

    AudioBuffer<float> buffer(channels, num_channels, inNumSamples);

    float peak = 0.0f;

    for (int ch = 0; ch < num_channels; ch++)
        peak = jmax(peak, buffer.getMagnitude(ch, 0, inNumSamples));

    float previous_peak = mPeakLevel.load();

    while (peak > previous_peak && !mPeakLevel.compare_exchange_weak(previous_peak, peak)) {
    }

    if (!mIsRecording.load())
        return;

    // The writers were made for the channel count we started with, and want exactly that many
    // pointers: extra channels are dropped, and a block that arrives narrower than expected has its
    // last channel repeated, because handing a wide writer a narrow buffer would have it read the
    // null terminator of the channel array and copy from it.
    const int num_record_channels = jlimit(1, kMaxCaptureChannels, mNumRecordedChannels);

    for (int ch = num_channels; ch < num_record_channels; ch++)
        channels[ch] = channels[num_channels - 1];

    // ...and for that block length, no more either: a loopback packet is whatever WASAPI had
    // ready, which can be several times an audio device's block, so hand it over in pieces.
    const int max_block = jmax(1, mNumRecordedSamplesPerBlock);

    for (int start = 0; start < inNumSamples; start += max_block) {
        AudioBuffer<float> to_record(channels, num_record_channels, start, jmin(max_block, inNumSamples - start));
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

    const bool is_standalone = mProcessor->wrapperType == AudioProcessor::wrapperType_Standalone;

    // The standalone app and the plugin share one settings file, so each keeps its selection under
    // its own keys. "System Audio" is a standalone-only choice: inside a DAW that endpoint is the
    // host's own master output, so a selection made in the standalone must never be loaded here.
    mKeySuffix = is_standalone ? String() : "Plugin";

    const bool was_configured_before = mProperties->getBoolValue(kConfiguredKey + mKeySuffix, false);

    _loadSelection();
    mInitialised = true;

#if JUCE_WINDOWS
    // Point the standalone app at the computer's own output, once. Inside a DAW this is never
    // done: there the loopback endpoint is the host's master output, and recording that while the
    // plugin is monitored is a feedback loop.
    const bool system_audio_default_applied = mProperties->getBoolValue(kSystemAudioDefaultKey, false);

    if (is_standalone && !system_audio_default_applied) {
        _scanLoopbackEndpoints();

        if (!mLoopbackEndpointNames.isEmpty()) {
            mSelectedDriverName = kSystemAudioDriverName;
            // The scan returns the default playback device first.
            mSelectedInputDeviceName = mLoopbackEndpointNames[0];
            mSelectedChannelSet = 0;

            // Only marked as done once it actually happened: a run where the endpoint scan came up
            // empty (the Windows audio service still starting, say) must be free to try again.
            mProperties->setValue(kSystemAudioDefaultKey, true);
        }
    }
#endif

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
    if (!was_configured_before && is_standalone && mSelectedInputDeviceName.isEmpty()) {
        if (auto* type = _getSelectedDriver()) {
            type->scanForDevices();

            const auto device_names = type->getDeviceNames(true);
            const int default_index = type->getDefaultDeviceIndex(true);

            if (isPositiveAndBelow(default_index, device_names.size()))
                mSelectedInputDeviceName = device_names[default_index];
        }
    }

    mProperties->setValue(kConfiguredKey + mKeySuffix, true);
    _saveSelection();
}

void AudioInputManager::ensureInitialised()
{
    _ensureInitialised();
}

bool AudioInputManager::_isLoopbackDriverSelected() const
{
#if JUCE_WINDOWS
    return mSelectedDriverName == kSystemAudioDriverName;
#else
    return false;
#endif
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

#if JUCE_WINDOWS
    if (_isLoopbackDriverSelected())
        return _openLoopbackDevice();
#endif

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
#if JUCE_WINDOWS
    // Unconditional: a driver change closes the old selection before the new driver name is in
    // place, so both kinds of stream have to be let go of here.
    _closeLoopbackDevice();
#endif

    mDeviceManager.removeAudioCallback(this);
    mDeviceManager.closeAudioDevice();
    mPeakLevel.store(0.0f);
}

#if JUCE_WINDOWS
void AudioInputManager::_scanLoopbackEndpoints()
{
    mLoopbackEndpointNames.clear();
    mLoopbackEndpointIds.clear();

    for (const auto& endpoint: okstudio::capture::WasapiLoopback::endpoints()) {
        mLoopbackEndpointNames.add(endpoint.name);
        mLoopbackEndpointIds.add(endpoint.id);
    }
}

String AudioInputManager::_getSelectedLoopbackEndpointId()
{
    int index = mLoopbackEndpointNames.indexOf(mSelectedInputDeviceName);

    if (index < 0) {
        // The saved endpoint may have appeared, or come back, since the last scan.
        _scanLoopbackEndpoints();
        index = mLoopbackEndpointNames.indexOf(mSelectedInputDeviceName);
    }

    return isPositiveAndBelow(index, mLoopbackEndpointIds.size()) ? mLoopbackEndpointIds[index] : String();
}

bool AudioInputManager::_openLoopbackDevice()
{
    const auto endpoint_id = _getSelectedLoopbackEndpointId();

    if (endpoint_id.isEmpty()) {
        mLastError = mSelectedInputDeviceName + " is not available to record from.";
        return false;
    }

    if (mLoopback != nullptr) {
        // Never replace the stream during a take: the writers were sized for the channel count and
        // sample rate it opened with, and an endpoint that comes back in a different format (a
        // headset renegotiating from stereo to mono, say) would not match them any more.
        if (mIsRecording.load())
            return !mLoopback->hasFailed();

        if (!mLoopback->hasFailed() && mOpenLoopbackEndpointId == endpoint_id)
            return true;

        _closeLoopbackDevice();
    }

    mLastError.clear();
    mLoopbackFailed.store(false);

    auto loopback = std::make_unique<LoopbackCapture>(*this);

    // Capped at two: the recorder only ever keeps a stereo pair, and taking the extra channels of
    // a surround endpoint off the capture thread costs for nothing.
    const auto result = loopback->start(endpoint_id, kLoopbackChannelCap);

    if (result.failed()) {
        mLastError = result.getErrorMessage();
        return false;
    }

    mLoopback = std::move(loopback);
    mOpenLoopbackEndpointId = endpoint_id;
    mPeakLevel.store(0.0f);
    return true;
}

void AudioInputManager::_closeLoopbackDevice()
{
    if (mLoopback != nullptr) {
        mLoopback->stop(); // Joins the capture thread, so no block can arrive after this returns.
        mLoopback.reset();
    }

    mOpenLoopbackEndpointId.clear();
    mLoopbackFailed.store(false);
    mPeakLevel.store(0.0f);
}
#endif

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

    mProperties->setValue(kDriverKey + mKeySuffix, mSelectedDriverName);
    mProperties->setValue(kDeviceKey + mKeySuffix, mSelectedInputDeviceName);
    mProperties->setValue(kChannelSetKey + mKeySuffix, mSelectedChannelSet);
    mProperties->saveIfNeeded();
}

void AudioInputManager::_loadSelection()
{
    if (mProperties == nullptr)
        return;

    mSelectedDriverName = mProperties->getValue(kDriverKey + mKeySuffix, {});
    mSelectedInputDeviceName = mProperties->getValue(kDeviceKey + mKeySuffix, {});
    mSelectedChannelSet = mProperties->getIntValue(kChannelSetKey + mKeySuffix, 0);
}
