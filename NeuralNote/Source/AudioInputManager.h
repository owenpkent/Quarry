//
// Audio input device selection and direct recording for NeuralNoteVideo.
//

#ifndef AudioInputManager_h
#define AudioInputManager_h

#include <JuceHeader.h>

class NeuralNoteAudioProcessor;

/**
 * Owns a dedicated, input-only audio device so NeuralNoteVideo can record straight from the
 * computer's own audio hardware, without a DAW and without the standalone app's Audio/MIDI
 * Settings dialog.
 *
 * This device is deliberately separate from the one the host (or the standalone wrapper) owns:
 * it means the same input picker behaves identically in a DAW and in the standalone app, and
 * selecting an input here never disturbs the host's own audio setup.
 *
 * While an input device is selected, it is only actually opened when the audio input panel is
 * on screen (so the level meter can show signal) or while recording, so NeuralNoteVideo never
 * silently holds a microphone open in the background.
 *
 * When no device is selected, recording falls back to the audio the host sends to the plugin,
 * which is the original NeuralNote behaviour.
 */
class AudioInputManager : private juce::AudioIODeviceCallback
{
public:
    explicit AudioInputManager(NeuralNoteAudioProcessor* inProcessor);

    ~AudioInputManager() override;

    /** Item shown in the input list for "record whatever the host sends us instead". */
    static const String& getHostInputName();

    /** Names of the available drivers, e.g. "Windows Audio", "ASIO", "DirectSound". */
    StringArray getDriverNames();

    String getSelectedDriverName() const { return mSelectedDriverName; }

    /** Selecting a driver clears the selected input device, as the list of inputs changes. */
    void setSelectedDriverName(const String& inDriverName);

    /** Input device names for the currently selected driver. */
    StringArray getInputDeviceNames();

    /** Empty when recording should use the host's audio (see getHostInputName()). */
    String getSelectedInputDeviceName() const { return mSelectedInputDeviceName; }

    /** Pass an empty string to go back to recording the host's audio. */
    void setSelectedInputDeviceName(const String& inDeviceName);

    bool hasSelectedInputDevice() const { return mSelectedInputDeviceName.isNotEmpty(); }

    /** Descriptions of the channel(s) of the selected device that can be recorded. */
    StringArray getInputChannelSetNames();

    /** Index into getInputChannelSetNames(), or -1 when there's nothing to choose from. */
    int getSelectedInputChannelSet() const { return mSelectedChannelSet; }

    void setSelectedInputChannelSet(int inIndex);

    /** True while the selected input device is actually open. */
    bool isInputDeviceOpen() const;

    /** Sample rate of the open input device, or 0 if no device is open. */
    double getCurrentSampleRate() const;

    /** Empty unless opening the selected input device failed. */
    String getLastError() const { return mLastError; }

    /**
     * Tell the manager whether the audio input panel is on screen. The selected device is held
     * open while it is, so the panel can show a live input level.
     */
    void setPanelOnScreen(bool inIsOnScreen);

    /** Peak level in [0, 1] seen since the previous call. */
    float getAndResetPeakLevel();

    /**
     * Open the selected input device (if needed) and start recording from it.
     * @return false if there is no selected device or it could not be opened, in which case
     *         getLastError() says why and nothing has been started.
     */
    bool startRecording();

    void stopRecording();

    bool isRecording() const { return mIsRecording.load(); }

    /** Drop out of recording without finalising it, for NeuralNoteAudioProcessor::clear(). */
    void abortRecording();

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(AudioIODevice* device) override;

    void audioDeviceStopped() override;

    void audioDeviceError(const String& errorMessage) override;

    void _ensureInitialised();

    AudioIODeviceType* _getSelectedDriver();

    /** @return true if the selected device ends up open. */
    bool _openInputDevice();

    void _closeInputDevice();

    /** Channels of the open device to record, as chosen with setSelectedInputChannelSet(). */
    BigInteger _getInputChannelMask(int inNumDeviceInputChannels) const;

    void _saveSelection() const;

    void _loadSelection();

    NeuralNoteAudioProcessor* mProcessor;

    juce::AudioDeviceManager mDeviceManager;
    std::unique_ptr<juce::PropertiesFile> mProperties;

    bool mInitialised = false;
    // The device manager is only initialised when a device is first opened: initialising it opens
    // whatever device it lands on, and NeuralNoteVideo should not do that just to list inputs.
    bool mDeviceManagerInitialised = false;
    bool mPanelOnScreen = false;

    String mSelectedDriverName;
    String mSelectedInputDeviceName;
    int mSelectedChannelSet = 0;

    String mLastError;

    // Number of channels the recording was started with, so a device change mid-recording (which
    // the UI disallows anyway) can never hand the writers more channels than they were made for.
    int mNumRecordedChannels = 1;

    std::atomic<bool> mIsRecording {false};
    std::atomic<float> mPeakLevel {0.0f};

    static constexpr int kMaxCaptureChannels = 32;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioInputManager)
};

#endif // AudioInputManager_h
