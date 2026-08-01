//
// Drop-down panel to pick an audio input and record from it.
//

#ifndef AudioInputView_h
#define AudioInputView_h

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "QuarryTooltips.h"
#include "UIDefines.h"

/**
 * The SOURCE strip: docked under the toolbar, picks the audio input Quarry records
 * from, and shows its level so it is obvious whether signal is arriving before hitting
 * record. Always on screen, because what you are about to record is not something you
 * should have to open a panel to check.
 */
class AudioInputView
    : public Component
    , public Timer
{
public:
    explicit AudioInputView(QuarryAudioProcessor& inProcessor);

    ~AudioInputView() override;

    void resized() override;

    void paint(Graphics& g) override;

    void visibilityChanged() override;

    void timerCallback() override;

    /** Refresh the drivers, inputs and channels shown, keeping the current selection. */
    void refresh();

    /** Called by the main view when the plugin state changes, to lock the pickers while recording. */
    void updateEnablements();

private:
    void _driverChanged();

    void _inputDeviceChanged();

    void _channelsChanged();

    /**
     * True while the open device is the selected one, so what the open device answers about itself
     * may be written back as the selection's. Being listed is not being open: a device that is there
     * but busy leaves some other device of the driver's open in its place.
     */
    bool _isSelectedDeviceOpen() const;

    String _getStatusText() const;

    /** What the panel would say if nothing had gone wrong: the plain state of the input. */
    String _getStateText() const;

    QuarryAudioProcessor& mProcessor;

    // False in a plugin, which never opens an audio device of its own: the driver, input and
    // channel pickers are hidden there, and the level meter and status text take their place.
    const bool mCanSelectInput;

    std::unique_ptr<ComboBox> mDriverDropDown;
    std::unique_ptr<ComboBox> mInputDropDown;
    std::unique_ptr<ComboBox> mChannelsDropDown;

    // Laid out in resized() and drawn in paint(), so the two cannot drift apart.
    Rectangle<int> mMeterBounds;
    Rectangle<int> mStatusBounds;

    // The entries of mInputDropDown, in the same order. Index 0 is the host input, which has no id,
    // and the rest are devices. Kept because a picked entry is passed back by id, not by name.
    Array<AudioInputManager::InputDevice> mInputDevices;

    bool mIsRefreshing = false;

    // False while the selected input device is missing from the list, when the device manager can
    // have fallen back to another device and nothing about that one may be written back as ours.
    bool mSelectedDeviceIsPresent = true;

    float mLevel = 0.0f;
    String mStatusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioInputView)
};

#endif // AudioInputView_h
