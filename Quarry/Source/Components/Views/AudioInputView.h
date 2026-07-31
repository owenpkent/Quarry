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
 * The AUDIO INPUT panel: drops down from the toolbar, picks the audio input Quarry
 * records from, and shows its level so it's obvious whether signal is arriving before hitting
 * record. Hidden by default; QuarryMainView owns it and toggles it.
 */
class AudioInputView
    : public Component
    , public Timer
{
public:
    /**
     * @param inProcessor The processor
     * @param inOnRecordClicked Called when the panel's record button is clicked. Routed back to
     *        the toolbar record button so there is only ever one way in and out of recording.
     */
    AudioInputView(QuarryAudioProcessor& inProcessor, std::function<void()> inOnRecordClicked);

    ~AudioInputView() override;

    void resized() override;

    void paint(Graphics& g) override;

    void visibilityChanged() override;

    void timerCallback() override;

    /** Refresh the drivers, inputs and channels shown, keeping the current selection. */
    void refresh();

    /** Called by the main view when the plugin state changes, to lock the pickers while recording. */
    void updateEnablements();

    std::function<void()> onCloseClicked;

private:
    void _driverChanged();

    void _inputDeviceChanged();

    void _channelsChanged();

    String _getStatusText() const;

    QuarryAudioProcessor& mProcessor;
    std::function<void()> mOnRecordClicked;

    std::unique_ptr<ComboBox> mDriverDropDown;
    std::unique_ptr<ComboBox> mInputDropDown;
    std::unique_ptr<ComboBox> mChannelsDropDown;

    std::unique_ptr<TextButton> mRecordButton;
    std::unique_ptr<ShapeButton> mCloseButton;

    // Input names as shown in mInputDropDown. Index 0 is the host input, the rest are devices.
    StringArray mInputDeviceNames;

    bool mIsRefreshing = false;

    float mLevel = 0.0f;
    String mStatusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioInputView)
};

#endif // AudioInputView_h
