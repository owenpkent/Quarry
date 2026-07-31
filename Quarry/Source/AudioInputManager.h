//
// Audio input device selection and direct recording for Quarry.
//

#ifndef AudioInputManager_h
#define AudioInputManager_h

#include <JuceHeader.h>

class QuarryAudioProcessor;

/**
 * Owns a dedicated, input-only audio device so Quarry can record straight from the
 * computer's own audio hardware, without a DAW and without the standalone app's Audio/MIDI
 * Settings dialog.
 *
 * All of that is standalone-only, see canSelectInputDevice(). A hosted plugin never lists drivers,
 * never lists devices and never opens one: a driver taken here is a driver the host's own engine
 * can lose, as an ASIO driver serves one client at a time. In a plugin there is nothing to select
 * and recording always uses the audio the host sends us, which is the original NeuralNote
 * behaviour.
 *
 * In the standalone app the device is deliberately separate from the one the wrapper owns, so
 * selecting an input here never disturbs the app's own audio setup.
 *
 * While an input device is selected, it is only actually opened when the audio input panel is
 * on screen (so the level meter can show signal) or while recording, so Quarry never
 * silently holds a microphone open in the background.
 *
 * When no device is selected, recording falls back to the audio the host sends to the plugin,
 * which is the original NeuralNote behaviour.
 *
 * On Windows there is one extra, synthetic driver called "System Audio", whose "input devices"
 * are the machine's playback endpoints: picking one records whatever the computer is playing,
 * through WASAPI loopback rather than through a juce::AudioIODevice. It behaves like any other
 * selection - same open-only-when-needed lifecycle, same level meter, same recorder.
 */
class AudioInputManager
    : private juce::AudioIODeviceCallback
    , private juce::AsyncUpdater
    , private juce::Timer
{
public:
    /** One entry of the input list: what identifies it, and what to show for it. */
    struct InputDevice
    {
        String id;
        String name;
    };

    explicit AudioInputManager(QuarryAudioProcessor* inProcessor);

    ~AudioInputManager() override;

    /** Item shown in the input list for "record whatever the host sends us instead". */
    static const String& getHostInputName();

    /**
     * False in a hosted plugin, where Quarry never opens an audio device of its own: everything
     * below that lists or selects one does nothing, and the panel hides its pickers.
     */
    bool canSelectInputDevice() const;

    /**
     * Load the saved selection and, in the standalone app on its first run, pick a default input.
     * Called by anything that asks a question whose answer depends on the selection, and by
     * QuarryAudioProcessor::startRecording() before it decides where to record from: the panel
     * may never have been opened, and until this has run there is no selection at all.
     * Does nothing in a plugin, which has no selection to load.
     */
    void ensureInitialised();

    /** Names of the available drivers, e.g. "Windows Audio", "ASIO", "DirectSound". */
    StringArray getDriverNames();

    String getSelectedDriverName() const { return mSelectedDriverName; }

    /** Selecting a driver clears the selected input device, as the list of inputs changes. */
    void setSelectedDriverName(const String& inDriverName);

    /**
     * The inputs of the selected driver, in the order the picker should show them. The name is for
     * display only: two "System Audio" endpoints can carry the same one, and any of them can be
     * renamed in Windows, so the id is what a selection is made and remembered by.
     */
    Array<InputDevice> getInputDevices();

    /** Empty when recording should use the host's audio (see getHostInputName()). */
    String getSelectedInputDeviceId() const { return mSelectedInputDeviceId; }

    /** The selected input as it should be shown to a person, never a raw endpoint id. */
    String getSelectedInputDeviceDisplayName() const { return mSelectedInputDeviceName; }

    /** Pass a default-constructed InputDevice to go back to recording the host's audio. */
    void setSelectedInputDevice(const InputDevice& inDevice);

    /** Always false in a plugin, so recording there always goes through the host's audio. */
    bool hasSelectedInputDevice() const { return _isStandalone() && mSelectedInputDeviceId.isNotEmpty(); }

    /** Descriptions of the channel(s) of the selected device that can be recorded. */
    StringArray getInputChannelSetNames();

    /** Index into getInputChannelSetNames(), or -1 when there's nothing to choose from. */
    int getSelectedInputChannelSet() const { return mSelectedChannelSet; }

    void setSelectedInputChannelSet(int inIndex);

    /** True while the selected input device is actually open. */
    bool isInputDeviceOpen() const;

    /** Sample rate of the open input device, or 0 if no device is open. */
    double getCurrentSampleRate() const;

    /**
     * Empty unless opening the selected input device failed, or it died while open. Always a
     * complete sentence, so the panel can show it as it is.
     */
    String getLastError();

    /**
     * The driver's own wording for the last thing it said about the open device, or empty. For
     * diagnosis only: a driver reports statuses it carries on through without missing a block, so
     * this is never a failure by itself and must never stand in for the panel's own state message.
     * Cleared when the device is next opened.
     */
    String getLastDriverMessage() const { return mDriverMessage; }

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

    /** Drop out of recording without finalising it, for QuarryAudioProcessor::clear(). */
    void abortRecording();

private:
    /**
     * What is being captured, as anything that judges a capture compares captures: a device that
     * stopped and started again as itself is a restart to carry on through, and anything else is a
     * different input than the one a take was started on.
     */
    struct CaptureIdentity
    {
        String name;
        double sampleRate = 0.0;
        int numInputChannels = 0;

        /** False when nothing is running, which is not the same as running as something else. */
        bool isValid() const { return sampleRate > 0.0; }

        bool matches(const CaptureIdentity& inOther) const;
    };

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(AudioIODevice* device) override;

    void audioDeviceStopped() override;

    void audioDeviceError(const String& errorMessage) override;

    /**
     * Message thread, where everything a capture thread noticed lands: nothing else can be done
     * from the thread it arrives on. A death, and a device that came back as a different one, are
     * acted on at once. A stop or an error that may yet turn out to be nothing starts the grace
     * period that decides which of the two it was.
     */
    void handleAsyncUpdate() override;

    /** Message thread. The grace period is up, so a deferred verdict on the capture is made. */
    void timerCallback() override;

#if JUCE_WINDOWS
    /**
     * Owns the WASAPI loopback stream and is its sink. Defined in the .cpp, because the Windows
     * audio headers it needs must not be pulled into everything that includes this one: they
     * define a Rectangle() that collides with juce::Rectangle.
     */
    struct LoopbackCapture;
#endif

    /**
     * The one place captured input goes: peak metering, then the recorder. Every capture path
     * (audio device callback, WASAPI loopback thread) hands its blocks to this and nothing else.
     * Called on a capture thread, so it allocates nothing and takes no lock of its own.
     */
    void _processInputBlock(const float* const* inChannelData, int inNumChannels, int inNumSamples);

    /**
     * The capture stream died. Called on a capture thread by every path that can notice it, so it
     * only raises a flag and hands the rest to handleAsyncUpdate() on the message thread.
     */
    void _reportCaptureFailure();

    /**
     * An audio device stopped, or reported an error. Neither on its own means the capture is gone:
     * a driver stops and starts the callback around a restart of its own (a shared-mode format
     * change, a headset switching profile) and reports statuses it carries on through. So the
     * verdict is left to timerCallback(), which makes it on what happened in the meantime, and on
     * whether what came back is the same device in the same format.
     */
    void _armCaptureVerdict();

    /** Message thread. Reports the dead capture, ends any take it fed, and drops the device. */
    void _endDeadCapture();

    /**
     * Message thread. End a take whose input is not the one it was started on any more: the writers
     * and the downsampler were made for that device in that format, and neither can be moved under
     * them. With no take running there is nothing to end, and the new input is simply what is
     * running now. @return true if a take was ended.
     */
    bool _endTakeIfCaptureChanged();

    /** Which way the take's input changed, as a complete sentence. */
    String _describeCaptureChange(const CaptureIdentity& inNow) const;

    /** Take whatever the driver last said off the thread it arrived on. */
    void _takeDriverMessage();

    static CaptureIdentity _identityOf(AudioIODevice* inDevice);

    void _setRunningIdentity(const CaptureIdentity& inIdentity);

    CaptureIdentity _getRunningIdentity();

    void _ensureInitialised();

    /**
     * The one gate on touching audio hardware: every path that lists drivers, lists devices, opens
     * one or remembers a selection is behind this, so a hosted plugin does none of it.
     */
    bool _isStandalone() const;

    AudioIODeviceType* _getSelectedDriver();

    /** True when the synthetic "System Audio" driver is the selected one. Never initialises. */
    bool _isLoopbackDriverSelected() const;

    /** @return true if the selected device ends up open. */
    bool _openInputDevice();

    void _closeInputDevice();

#if JUCE_WINDOWS
    /** Refresh mLoopbackEndpointNames / mLoopbackEndpointIds, default playback endpoint first. */
    void _scanLoopbackEndpoints();

    /** The selected loopback endpoint's id, or empty if that endpoint isn't there any more. */
    String _getSelectedLoopbackEndpointId();

    /**
     * Turn a selection kept by endpoint name (how a build before this one kept it) into the id that
     * name belongs to. Wants a current scan, and is run against every one of them rather than only
     * the one at load: the output may only have been switched on after the app started.
     */
    void _migrateLoopbackNameToId();

    /**
     * Take the shown name of the selected endpoint from a scan of the endpoints. The selection is
     * kept by id, and Windows lets an output be renamed under the same one, so everything that
     * shows the name would otherwise go on showing the one it had when it was picked.
     */
    void _refreshSelectedDeviceName(const String& inName);

    bool _openLoopbackDevice();

    void _closeLoopbackDevice();
#endif

    /**
     * Drop a channel selection a device this wide cannot honour. The same device can come back with
     * fewer inputs than it had when the choice was made, and a selection kept past that would record
     * channels other than the ones the panel shows.
     */
    void _clampSelectedChannelSet(int inNumDeviceInputChannels);

    /** Channels of the open device to record, as chosen with setSelectedInputChannelSet(). */
    BigInteger _getInputChannelMask(int inNumDeviceInputChannels) const;

    void _saveSelection() const;

    void _loadSelection();

    QuarryAudioProcessor* mProcessor;

    // Never asked for a device type or a device in a plugin, where _isStandalone() is false.
    juce::AudioDeviceManager mDeviceManager;

    // Only created in the standalone app. It is one file per machine, so a plugin remembering its
    // selection in it would hand every other instance on the machine that same device.
    std::unique_ptr<juce::PropertiesFile> mProperties;

    bool mInitialised = false;
    // The device manager is only initialised when a device is first opened: initialising it opens
    // whatever device it lands on, and Quarry should not do that just to list inputs.
    bool mDeviceManagerInitialised = false;
    bool mPanelOnScreen = false;

    String mSelectedDriverName;

    // What the selection is: the device's own name for an audio driver, the WASAPI endpoint id for
    // "System Audio". The name is only ever shown, never matched against.
    String mSelectedInputDeviceId;
    String mSelectedInputDeviceName;

    int mSelectedChannelSet = 0;

    String mLastError;

    // Number of channels the recording was started with, so a device change mid-recording (which
    // the UI disallows anyway) can never hand the writers more channels than they were made for.
    int mNumRecordedChannels = 1;

    // Likewise for length: the writers' internal buffers were sized for this many samples, and a
    // loopback packet can be far longer than an audio device's block, so blocks are cut to fit.
    int mNumRecordedSamplesPerBlock = 512;

    std::atomic<bool> mIsRecording {false};
    std::atomic<float> mPeakLevel {0.0f};

    // Set from a capture thread when the stream dies, so no String is touched and no take is ended
    // there. Read by handleAsyncUpdate(), which does both.
    std::atomic<bool> mCaptureFailed {false};

    // True between audioDeviceAboutToStart() and audioDeviceStopped(): how a deferred verdict tells
    // a device that restarted itself from one that has gone away.
    std::atomic<bool> mDeviceRunning {false};

    // A stop or an error is waiting for handleAsyncUpdate() to start the grace period on it.
    std::atomic<bool> mVerdictPending {false};

    // Blocks the capture has handed over, only ever compared against itself: a device that has
    // delivered audio since the verdict was armed is a device that is still there.
    std::atomic<uint32_t> mCaptureBlockCount {0};
    uint32_t mBlockCountWhenArmed = 0;

    // What is being captured right now, written from the thread a device starts and stops on.
    SpinLock mCaptureIdentityLock;
    CaptureIdentity mRunningIdentity;

    // What the take now recording was started on. Message thread only, like every take there is.
    CaptureIdentity mTakeIdentity;

    // The driver's own wording for the last error it reported, which arrives on its own thread.
    SpinLock mDriverErrorLock;
    String mDriverError;

    // The same wording, once the message thread has taken it. Kept well apart from mLastError: a
    // driver reports statuses it recovers from without missing a block, and one of those must never
    // take the place of what the panel would otherwise be saying about a device that is working.
    String mDriverMessage;

#if JUCE_WINDOWS
    // Only alive while the loopback "device" is open, which is the same window of time a
    // juce::AudioIODevice would be open for: panel on screen, or recording.
    std::unique_ptr<LoopbackCapture> mLoopback;
    String mOpenLoopbackEndpointId;

    // Last scan of the playback endpoints: the names the picker shows, made unique so two endpoints
    // called the same thing can be told apart, and the WASAPI endpoint ids, in the same order.
    StringArray mLoopbackEndpointNames;
    StringArray mLoopbackEndpointIds;
#endif

    static constexpr int kMaxCaptureChannels = 32;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioInputManager)
};

#endif // AudioInputManager_h
