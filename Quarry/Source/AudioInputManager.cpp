//
// Audio input device selection and direct recording for Quarry.
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
// What the device is, and separately what it is called: for "System Audio" the first is a WASAPI
// endpoint id and the second is a name Windows lets the user change.
const String kDeviceKey = "inputDevice";
const String kDeviceNameKey = "inputDeviceName";
const String kChannelSetKey = "inputChannelSet";
const String kConfiguredKey = "inputConfigured";
const String kSystemAudioDefaultKey = "inputSystemAudioDefaultApplied";

// How long a stop or an error from an audio device is given to turn out to have been nothing. JUCE
// closes and reopens a device around a format change and the callback starts again within
// milliseconds, so this is far more than a restart needs, and still short enough that a take which
// really has lost its input ends while the person recording is still watching it.
constexpr int kCaptureVerdictGraceMs = 1000;

// How far apart two sample rates have to be to count as a different format. A rate comes from the
// driver either side of a restart, and ending a take over the last digit of one that never really
// changed would be worse than the change it was watching for.
constexpr double kSampleRateMatchToleranceHz = 1.0;

// The rate as the panel writes it, so a message about a format change reads the same way.
String sampleRateText(double inSampleRate)
{
    return String(inSampleRate / 1000.0, 1) + " kHz";
}

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

    void loopbackFailed() override { mOwner._reportCaptureFailure(); }

    AudioInputManager& mOwner;
    okstudio::capture::WasapiLoopback mStream;
};
#endif

AudioInputManager::AudioInputManager(QuarryAudioProcessor* inProcessor)
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

bool AudioInputManager::canSelectInputDevice() const
{
    return _isStandalone();
}

StringArray AudioInputManager::getDriverNames()
{
    StringArray names;

    if (!_isStandalone())
        return names;

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
    if (!_isStandalone() || inDriverName == mSelectedDriverName)
        return;

    // The inputs of the previous driver mean nothing to the new one.
    setSelectedInputDevice({});

    mSelectedDriverName = inDriverName;
    _saveSelection();
}

Array<AudioInputManager::InputDevice> AudioInputManager::getInputDevices()
{
    Array<InputDevice> devices;

    if (!_isStandalone())
        return devices;

#if JUCE_WINDOWS
    if (_isLoopbackDriverSelected()) {
        _ensureInitialised();
        _scanLoopbackEndpoints();
        _migrateLoopbackNameToId();

        for (int i = 0; i < mLoopbackEndpointIds.size(); i++) {
            devices.add({mLoopbackEndpointIds[i], mLoopbackEndpointNames[i]});

            if (mLoopbackEndpointIds[i] == mSelectedInputDeviceId)
                _refreshSelectedDeviceName(mLoopbackEndpointNames[i]);
        }

        return devices;
    }
#endif

    if (auto* type = _getSelectedDriver()) {
        type->scanForDevices();

        // A driver's device name is how the driver itself is asked for that device, so it is both.
        for (const auto& name: type->getDeviceNames(true))
            devices.add({name, name});
    }

    return devices;
}

void AudioInputManager::setSelectedInputDevice(const InputDevice& inDevice)
{
    if (!_isStandalone() || inDevice.id == mSelectedInputDeviceId)
        return;

    jassert(!mIsRecording.load()); // The UI locks the pickers while recording.

    _closeInputDevice();

    mSelectedInputDeviceId = inDevice.id;
    mSelectedInputDeviceName = inDevice.name.isNotEmpty() ? inDevice.name : inDevice.id;
    mSelectedChannelSet = 0;
    mLastError.clear();
    _saveSelection();

    if (mPanelOnScreen && mSelectedInputDeviceId.isNotEmpty())
        _openInputDevice();
}

StringArray AudioInputManager::getInputChannelSetNames()
{
    StringArray names;

    if (!_isStandalone())
        return names;

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
    if (!_isStandalone() || inIndex == mSelectedChannelSet || inIndex < 0)
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
    return mLastError;
}

void AudioInputManager::setPanelOnScreen(bool inIsOnScreen)
{
    if (inIsOnScreen == mPanelOnScreen)
        return;

    mPanelOnScreen = inIsOnScreen;

    if (mPanelOnScreen) {
        _ensureInitialised();

        if (mSelectedInputDeviceId.isNotEmpty())
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

    // Whatever went wrong last time has been superseded by this take, and an open that fails below
    // says so again.
    mLastError.clear();

    if (mSelectedInputDeviceId.isEmpty())
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

        // A loopback stream is never swapped under a take (see _openLoopbackDevice) and reports its
        // own death outright, so it has no identity for a verdict to weigh.
        mTakeIdentity = {};

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

    // What the take is being recorded from, taken from the same place a restart's identity comes
    // from so the two can only ever differ for a real reason. The device itself is only asked
    // directly if the callback has somehow not been told the device started.
    mTakeIdentity = _getRunningIdentity();

    if (!mTakeIdentity.isValid())
        mTakeIdentity = _identityOf(device);

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
    mTakeIdentity = {};
    mProcessor->getSourceAudioManager()->stopRecording();

    if (!mPanelOnScreen)
        _closeInputDevice();
}

void AudioInputManager::abortRecording()
{
    mIsRecording.store(false);
    mTakeIdentity = {};

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

    mCaptureBlockCount.fetch_add(1, std::memory_order_relaxed);

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

bool AudioInputManager::CaptureIdentity::matches(const CaptureIdentity& inOther) const
{
    return name == inOther.name && std::abs(sampleRate - inOther.sampleRate) < kSampleRateMatchToleranceHz
           && numInputChannels == inOther.numInputChannels;
}

AudioInputManager::CaptureIdentity AudioInputManager::_identityOf(AudioIODevice* inDevice)
{
    CaptureIdentity identity;

    if (inDevice != nullptr) {
        identity.name = inDevice->getName();
        identity.sampleRate = inDevice->getCurrentSampleRate();
        identity.numInputChannels = inDevice->getActiveInputChannels().countNumberOfSetBits();
    }

    return identity;
}

void AudioInputManager::_setRunningIdentity(const CaptureIdentity& inIdentity)
{
    const SpinLock::ScopedLockType lock(mCaptureIdentityLock);
    mRunningIdentity = inIdentity;
}

AudioInputManager::CaptureIdentity AudioInputManager::_getRunningIdentity()
{
    const SpinLock::ScopedLockType lock(mCaptureIdentityLock);
    return mRunningIdentity;
}

void AudioInputManager::audioDeviceAboutToStart(AudioIODevice* device)
{
    // Kept, not dropped: "something is running again" and "the take's own input is running again"
    // are different answers, and only the second one lets a take carry on.
    _setRunningIdentity(_identityOf(device));

    mPeakLevel.store(0.0f);
    mDeviceRunning.store(true);

    // What started may not be what a take is being recorded from, and that is worth settling now
    // rather than after a grace period spent writing the wrong input into it.
    if (mIsRecording.load())
        triggerAsyncUpdate();
}

void AudioInputManager::audioDeviceStopped()
{
    _setRunningIdentity({});

    mPeakLevel.store(0.0f);
    mDeviceRunning.store(false);

    // Not the end of anything by itself: this is equally how JUCE's own restart of a device begins,
    // and how our own close of one does, so what it was is decided a moment from now.
    _armCaptureVerdict();
}

void AudioInputManager::audioDeviceError(const String& errorMessage)
{
    {
        const SpinLock::ScopedLockType lock(mDriverErrorLock);
        mDriverError = errorMessage;
    }

    // Drivers send every status they have through here, including ones they recover from without
    // missing a block, so this never takes a device down on its own either.
    _armCaptureVerdict();
}

void AudioInputManager::_reportCaptureFailure()
{
    mCaptureFailed.store(true);
    triggerAsyncUpdate();
}

void AudioInputManager::_armCaptureVerdict()
{
    mVerdictPending.store(true);
    triggerAsyncUpdate();
}

void AudioInputManager::handleAsyncUpdate()
{
    // A capture path that can tell a death from anything else says so outright, and is believed.
    if (mCaptureFailed.exchange(false)) {
        _endDeadCapture();
        return;
    }

    // A device that has come back as something else is already certain, so a take on it ends here
    // and does not wait for the grace period, which is only there to decide the uncertain case.
    _endTakeIfCaptureChanged();

    if (mVerdictPending.exchange(false)) {
        mBlockCountWhenArmed = mCaptureBlockCount.load();
        startTimer(kCaptureVerdictGraceMs);
    }
}

void AudioInputManager::timerCallback()
{
    stopTimer();

    // Wherever this ends up, it is a note about the device and not a verdict on it: a driver reports
    // plenty it recovers from, so nothing here is a failure until the device is judged one.
    _takeDriverMessage();

    // The capture is only gone if it has neither started again nor delivered a block since: a
    // device JUCE restarted has done both by now, and one whose driver reported something it
    // recovered from never stopped feeding us at all.
    if (mDeviceRunning.load() && mCaptureBlockCount.load() != mBlockCountWhenArmed) {
        // Running again is not the same as running as the same thing, and only a take can tell the
        // difference: with none in progress the new input is simply the input now.
        _endTakeIfCaptureChanged();
        return;
    }

    _endDeadCapture();
}

bool AudioInputManager::_endTakeIfCaptureChanged()
{
    if (!mIsRecording.load() || !mTakeIdentity.isValid())
        return false;

    const auto running = _getRunningIdentity();

    // Nothing running at all is a death rather than a change, and is the deferred verdict's to make.
    if (!running.isValid() || running.matches(mTakeIdentity))
        return false;

    mLastError = _describeCaptureChange(running);

    // Out through the same door the stop button uses, so what was captured before the input changed
    // is finalised and transcribed rather than thrown away.
    mProcessor->stopRecording();
    return true;
}

String AudioInputManager::_describeCaptureChange(const CaptureIdentity& inNow) const
{
    const String ends = " The take ends where the input changed.";

    if (inNow.name != mTakeIdentity.name)
        return mSelectedInputDeviceName + " was replaced by " + inNow.name + " while recording." + ends;

    if (std::abs(inNow.sampleRate - mTakeIdentity.sampleRate) >= kSampleRateMatchToleranceHz)
        return mSelectedInputDeviceName + " came back at " + sampleRateText(inNow.sampleRate) + " instead of "
               + sampleRateText(mTakeIdentity.sampleRate) + " while recording." + ends;

    return mSelectedInputDeviceName + " came back with " + String(inNow.numInputChannels)
           + " input channels instead of " + String(mTakeIdentity.numInputChannels) + " while recording." + ends;
}

void AudioInputManager::_takeDriverMessage()
{
    String message;

    {
        const SpinLock::ScopedLockType lock(mDriverErrorLock);
        message.swapWith(mDriverError);
    }

    if (message.isNotEmpty())
        mDriverMessage = message;
}

void AudioInputManager::_endDeadCapture()
{
    _takeDriverMessage();

    const bool was_recording = mIsRecording.load();

    // Now that this is a failure, the driver's own wording is the only account of why, so it is
    // carried inside a sentence of ours rather than left to be read on its own.
    const String detail = mDriverMessage.isNotEmpty() ? " The driver said: " + mDriverMessage : String();

    // An open that failed has already said something more specific about why, and is only ever
    // spoken over by a take that this cut short.
    if (was_recording)
        mLastError =
            mSelectedInputDeviceName + " stopped while recording. The take ends where the input stopped." + detail;
    else if (mLastError.isEmpty())
        mLastError = mSelectedInputDeviceName + " stopped sending audio." + detail;

    // The take cannot be continued: what was feeding it is gone. End it through the same door the
    // stop button uses, so the audio captured so far is finalised and transcribed rather than lost.
    if (was_recording)
        mProcessor->stopRecording();

    // The stream is dead even if it still looks open, so let go of it: the next record (or the next
    // time the panel comes up) then opens it again from scratch.
    _closeInputDevice();
}

void AudioInputManager::_ensureInitialised()
{
    if (mInitialised)
        return;

    // A plugin has no selection: it records what the host sends it, so there is nothing to load,
    // nothing to remember, and no settings file to remember it in. That file is one per machine,
    // so a selection kept there would follow every other instance on the machine around.
    if (!_isStandalone()) {
        mInitialised = true;
        return;
    }

    PropertiesFile::Options options;
    options.applicationName = "QuarryAudioInput";
    options.filenameSuffix = "settings";
    options.folderName = "Quarry";
    options.osxLibrarySubFolder = "Application Support";
    mProperties = std::make_unique<PropertiesFile>(options);

    const bool was_configured_before = mProperties->getBoolValue(kConfiguredKey, false);

    _loadSelection();
    mInitialised = true;

#if JUCE_WINDOWS
    // Point the app at the computer's own output, once.
    const bool system_audio_default_applied = mProperties->getBoolValue(kSystemAudioDefaultKey, false);

    if (!system_audio_default_applied) {
        _scanLoopbackEndpoints();

        if (!mLoopbackEndpointIds.isEmpty()) {
            mSelectedDriverName = kSystemAudioDriverName;
            // The scan returns the default playback device first.
            mSelectedInputDeviceId = mLoopbackEndpointIds[0];
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
    if (!was_configured_before && mSelectedInputDeviceId.isEmpty()) {
        if (auto* type = _getSelectedDriver()) {
            type->scanForDevices();

            const auto device_names = type->getDeviceNames(true);
            const int default_index = type->getDefaultDeviceIndex(true);

            if (isPositiveAndBelow(default_index, device_names.size())) {
                mSelectedInputDeviceId = device_names[default_index];
                mSelectedInputDeviceName = mSelectedInputDeviceId;
            }
        }
    }

    mProperties->setValue(kConfiguredKey, true);
    _saveSelection();
}

void AudioInputManager::ensureInitialised()
{
    _ensureInitialised();
}

bool AudioInputManager::_isStandalone() const
{
    return mProcessor->wrapperType == AudioProcessor::wrapperType_Standalone;
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

    // getAvailableDeviceTypes() scans every driver, which in a plugin means opening drivers the
    // host is already using, behind its back.
    if (!_isStandalone())
        return nullptr;

    for (auto* type: mDeviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == mSelectedDriverName)
            return type;

    return nullptr;
}

bool AudioInputManager::_openInputDevice()
{
    _ensureInitialised();

    // A plugin never opens a device of its own, whatever it was asked to open.
    if (!_isStandalone() || mSelectedInputDeviceId.isEmpty())
        return false;

#if JUCE_WINDOWS
    if (_isLoopbackDriverSelected())
        return _openLoopbackDevice();
#endif

    if (isInputDeviceOpen() && mDeviceManager.getAudioDeviceSetup().inputDeviceName == mSelectedInputDeviceId)
        return true;

    mLastError.clear();
    mDriverMessage.clear();

    if (!mDeviceManagerInitialised) {
        // Two input channels wanted, no outputs. This is the only call that can briefly touch a
        // device other than the chosen one, so name ours as the preferred default.
        mDeviceManager.initialise(2, 0, nullptr, false, mSelectedInputDeviceId);
        mDeviceManagerInitialised = true;
    }

    if (mDeviceManager.getCurrentAudioDeviceType() != mSelectedDriverName)
        mDeviceManager.setCurrentAudioDeviceType(mSelectedDriverName, true);

    auto setup = mDeviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = mSelectedInputDeviceId;
    setup.outputDeviceName = {};
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();

    const auto setup_error = mDeviceManager.setAudioDeviceSetup(setup, true);

    if (setup_error.isNotEmpty() || !isInputDeviceOpen()) {
        // The driver's message is a fragment, so it is only ever shown inside a sentence of ours.
        mLastError = setup_error.isNotEmpty() ? "Could not open " + mSelectedInputDeviceName + ": " + setup_error
                                              : "Could not open " + mSelectedInputDeviceName + ".";

        return false;
    }

    // Now the device is open we know how many channels it has, so the channel choice can be applied.
    if (auto* device = mDeviceManager.getCurrentAudioDevice()) {
        const int num_input_channels = device->getInputChannelNames().size();

        _clampSelectedChannelSet(num_input_channels);

        const auto mask = _getInputChannelMask(num_input_channels);

        if (mask.countNumberOfSetBits() > 0 && mask != device->getActiveInputChannels()) {
            setup = mDeviceManager.getAudioDeviceSetup();
            setup.useDefaultInputChannels = false;
            setup.inputChannels = mask;

            const auto channel_error = mDeviceManager.setAudioDeviceSetup(setup, true);

            if (channel_error.isNotEmpty())
                mLastError = "Could not record those channels: " + channel_error;
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

    // Nothing left to report a failure about: whatever was captured has been let go of. Closing is
    // itself a stop, and the last word on the one we just asked for is that we asked for it.
    mCaptureFailed.store(false);
    mVerdictPending.store(false);
    mDeviceRunning.store(false);
    _setRunningIdentity({});
    stopTimer();

    mDriverMessage.clear();

    const SpinLock::ScopedLockType lock(mDriverErrorLock);
    mDriverError.clear();
}

#if JUCE_WINDOWS
void AudioInputManager::_scanLoopbackEndpoints()
{
    // The single place the playback endpoints are enumerated, so gating it here keeps "System
    // Audio" out of a plugin: inside a DAW that endpoint is the host's own master output anyway.
    if (!_isStandalone())
        return;

    mLoopbackEndpointNames.clear();
    mLoopbackEndpointIds.clear();

    for (const auto& endpoint: okstudio::capture::WasapiLoopback::endpoints()) {
        auto name = endpoint.name;

        // Two endpoints can be called the same thing (identical DACs, an HDMI output that reports
        // no name of its own), and the list is all the user has to tell them apart by.
        for (int copy = 2; mLoopbackEndpointNames.contains(name); copy++)
            name = endpoint.name + " (" + String(copy) + ")";

        mLoopbackEndpointNames.add(name);
        mLoopbackEndpointIds.add(endpoint.id);
    }
}

String AudioInputManager::_getSelectedLoopbackEndpointId()
{
    // Always from a scan taken now, never from the last one: the endpoint may have appeared, come
    // back or been renamed in Windows since, and this is where the name every message about it
    // reads from. Only ever reached by opening the device, so it is nowhere near a hot path.
    _scanLoopbackEndpoints();
    _migrateLoopbackNameToId();

    const int index = mLoopbackEndpointIds.indexOf(mSelectedInputDeviceId);

    if (index < 0)
        return {};

    _refreshSelectedDeviceName(mLoopbackEndpointNames[index]);
    return mSelectedInputDeviceId;
}

void AudioInputManager::_migrateLoopbackNameToId()
{
    if (!_isLoopbackDriverSelected() || mSelectedInputDeviceId.isEmpty())
        return;

    // An id that is one of the scanned ids is a good id, and is left exactly as it is.
    if (mLoopbackEndpointIds.contains(mSelectedInputDeviceId))
        return;

    // A WASAPI endpoint id is never one of the shown names, so an id that is one of them is a name
    // left in the id field by a build that kept the selection that way, and only then is there
    // anything to migrate. A name matching nothing is left alone, so a renamed output reads as the
    // missing device it now is rather than quietly becoming a different one.
    const int index = mLoopbackEndpointNames.indexOf(mSelectedInputDeviceId);

    if (index < 0)
        return;

    mSelectedInputDeviceId = mLoopbackEndpointIds[index];
    mSelectedInputDeviceName = mLoopbackEndpointNames[index];
    _saveSelection();
}

void AudioInputManager::_refreshSelectedDeviceName(const String& inName)
{
    if (inName.isEmpty() || inName == mSelectedInputDeviceName)
        return;

    mSelectedInputDeviceName = inName;
    _saveSelection();
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
    mCaptureFailed.store(false);

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
    mPeakLevel.store(0.0f);
}
#endif

void AudioInputManager::_clampSelectedChannelSet(int inNumDeviceInputChannels)
{
    // What getInputChannelSetNames() lists for a device this wide: its stereo pairs, then each of
    // its channels on its own.
    const int num_sets = inNumDeviceInputChannels / 2 + inNumDeviceInputChannels;

    if (num_sets <= 0 || mSelectedChannelSet < num_sets)
        return;

    mSelectedChannelSet = num_sets - 1;
    _saveSelection();
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
    mProperties->setValue(kDeviceKey, mSelectedInputDeviceId);
    mProperties->setValue(kDeviceNameKey, mSelectedInputDeviceName);
    mProperties->setValue(kChannelSetKey, mSelectedChannelSet);
    mProperties->saveIfNeeded();
}

void AudioInputManager::_loadSelection()
{
    if (mProperties == nullptr)
        return;

    mSelectedDriverName = mProperties->getValue(kDriverKey, {});
    mSelectedInputDeviceId = mProperties->getValue(kDeviceKey, {});
    // A file written before endpoints were kept by id has no name of its own: back then the name
    // was the key, so it is both.
    mSelectedInputDeviceName = mProperties->getValue(kDeviceNameKey, mSelectedInputDeviceId);
    mSelectedChannelSet = jmax(0, mProperties->getIntValue(kChannelSetKey, 0));

#if JUCE_WINDOWS
    // Turn such a file's endpoint name into the endpoint id it names. Only worth a scan of its own
    // when there is a selection to migrate: every later scan tries again anyway, so an endpoint
    // that is not switched on until after this is still picked up when it appears.
    if (_isLoopbackDriverSelected() && mSelectedInputDeviceId.isNotEmpty()) {
        _scanLoopbackEndpoints();
        _migrateLoopbackNameToId();
    }
#endif
}
