//
// Drop-down panel to pick an audio input and record from it.
//

#include "AudioInputView.h"

namespace
{
constexpr int kLabelX = 16;
constexpr int kLabelWidth = 88;
constexpr int kControlX = 112;
constexpr int kRowHeight = 22;
constexpr int kDriverRowY = 44;
constexpr int kInputRowY = 74;
constexpr int kChannelsRowY = 104;
constexpr int kMeterY = 140;
constexpr int kMeterHeight = 16;
constexpr int kStatusY = 172;

// Where the meter and the status text sit when there are no pickers above them to make room for.
constexpr int kNoPickersMeterY = kDriverRowY;
constexpr int kNoPickersStatusY = kInputRowY;

// The primary target of the whole app, so it is sized to be hit with a mouse without aiming.
constexpr int kRecordButtonWidth = 180;
constexpr int kRecordButtonHeight = 72;
} // namespace

AudioInputView::AudioInputView(QuarryAudioProcessor& inProcessor, std::function<void()> inOnRecordClicked)
    : mProcessor(inProcessor)
    , mOnRecordClicked(std::move(inOnRecordClicked))
    , mCanSelectInput(inProcessor.getAudioInputManager()->canSelectInputDevice())
{
    auto make_drop_down = [this](const String& inName, const String& inTooltip, std::function<void()> inOnChange) {
        auto drop_down = std::make_unique<ComboBox>(inName);
        drop_down->setEditableText(false);
        drop_down->setJustificationType(Justification::centredLeft);
        drop_down->setTooltip(inTooltip);
        drop_down->onChange = std::move(inOnChange);
        addAndMakeVisible(*drop_down);
        return drop_down;
    };

    mDriverDropDown = make_drop_down("Driver", QuarryTooltips::ai_driver, [this] { _driverChanged(); });
    mInputDropDown = make_drop_down("Input", QuarryTooltips::ai_device, [this] { _inputDeviceChanged(); });
    mChannelsDropDown = make_drop_down("Channels", QuarryTooltips::ai_channels, [this] { _channelsChanged(); });

    // In a plugin there is nothing to pick: the manager lists no drivers and opens no devices, so
    // an empty picker would only invite the user to try.
    if (!mCanSelectInput) {
        mDriverDropDown->setVisible(false);
        mInputDropDown->setVisible(false);
        mChannelsDropDown->setVisible(false);
    }

    mRecordButton = std::make_unique<TextButton>("RECORD");
    mRecordButton->setTooltip(QuarryTooltips::record);
    mRecordButton->onClick = [this] {
        if (mOnRecordClicked != nullptr)
            mOnRecordClicked();
    };
    addAndMakeVisible(*mRecordButton);

    // A cross drawn as a path: the button is smaller than the look and feel's button font, so a
    // text "X" would come out as an ellipsis.
    Path cross;
    cross.startNewSubPath(0.0f, 0.0f);
    cross.lineTo(1.0f, 1.0f);
    cross.startNewSubPath(1.0f, 0.0f);
    cross.lineTo(0.0f, 1.0f);

    PathStrokeType(0.18f, PathStrokeType::curved, PathStrokeType::rounded).createStrokedPath(cross, cross);

    mCloseButton = std::make_unique<ShapeButton>("Close", BLACK, BLACK.withAlpha(0.6f), BLACK.withAlpha(0.4f));
    mCloseButton->setShape(cross, true, true, false);
    mCloseButton->setTooltip("Close");
    mCloseButton->onClick = [this] {
        if (onCloseClicked != nullptr)
            onCloseClicked();
    };
    addAndMakeVisible(*mCloseButton);
}

AudioInputView::~AudioInputView()
{
    stopTimer();

    // Closing the editor destroys the panel without hiding it first, and nothing else would ever
    // turn the host input metering back off: it would keep scanning every block for no one.
    mProcessor.setHostInputLevelWanted(false);
    mProcessor.getAudioInputManager()->setPanelOnScreen(false);
}

void AudioInputView::resized()
{
    const int control_width = getWidth() - kControlX - kLabelX;

    mDriverDropDown->setBounds(kControlX, kDriverRowY, control_width, kRowHeight);
    mInputDropDown->setBounds(kControlX, kInputRowY, control_width, kRowHeight);
    mChannelsDropDown->setBounds(kControlX, kChannelsRowY, control_width, kRowHeight);

    mCloseButton->setBounds(getWidth() - kLabelX - 22, 10, 22, 22);

    // Bottom right, below the meter, clamped so it can never overflow the panel.
    const int record_width = jmin(kRecordButtonWidth, getWidth() - 2 * kLabelX);
    const int record_height = jmin(kRecordButtonHeight, getHeight() - kStatusY - 10);

    mRecordButton->setBounds(getWidth() - kLabelX - record_width, kStatusY, record_width, record_height);
}

void AudioInputView::paint(Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    DropShadow(BLACK.withAlpha(0.4f), 12, {0, 3}).drawForRectangle(g, getLocalBounds().reduced(2));

    g.setColour(WHITE_SOLID);
    g.fillRoundedRectangle(bounds, 5.0f);
    g.setColour(BLACK.withAlpha(0.25f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 5.0f, 1.0f);

    g.setColour(BLACK);
    g.setFont(UIDefines::TITLE_FONT());
    g.drawText("AUDIO INPUT", Rectangle<int>(kLabelX, 8, 260, 20), Justification::centredLeft);

    g.setFont(UIDefines::LABEL_FONT());

    if (mCanSelectInput) {
        g.drawText("DRIVER", Rectangle<int>(kLabelX, kDriverRowY, kLabelWidth, kRowHeight), Justification::centredLeft);
        g.drawText("INPUT", Rectangle<int>(kLabelX, kInputRowY, kLabelWidth, kRowHeight), Justification::centredLeft);
        g.drawText(
            "CHANNELS", Rectangle<int>(kLabelX, kChannelsRowY, kLabelWidth, kRowHeight), Justification::centredLeft);
    }

    const int meter_y = mCanSelectInput ? kMeterY : kNoPickersMeterY;
    const int status_y = mCanSelectInput ? kStatusY : kNoPickersStatusY;

    g.drawText("LEVEL", Rectangle<int>(kLabelX, meter_y, kLabelWidth, kMeterHeight), Justification::centredLeft);

    // Level meter
    const int meter_width = getWidth() - kControlX - kLabelX;
    auto meter_bounds = Rectangle<int>(kControlX, meter_y, meter_width, kMeterHeight).toFloat();

    g.setColour(BLACK.withAlpha(0.12f));
    g.fillRoundedRectangle(meter_bounds, 3.0f);

    if (mLevel > 0.0f) {
        auto filled = meter_bounds.withWidth(meter_bounds.getWidth() * jmin(mLevel, 1.0f));

        g.setColour(mLevel > 0.95f ? RECORD_RED : BLACK.withAlpha(0.75f));
        g.fillRoundedRectangle(filled, 3.0f);
    }

    // Status text sits to the left of the record button, never underneath it.
    const int status_width = mRecordButton->getX() - kLabelX - 16;

    g.setColour(BLACK.withAlpha(0.7f));
    g.setFont(UIDefines::DROPDOWN_FONT());
    g.drawFittedText(mStatusText,
                     Rectangle<int>(kLabelX, status_y, status_width, getHeight() - status_y - 10),
                     Justification::topLeft,
                     4);
}

void AudioInputView::visibilityChanged()
{
    auto* input_manager = mProcessor.getAudioInputManager();

    mProcessor.setHostInputLevelWanted(isVisible());

    if (isVisible()) {
        input_manager->setPanelOnScreen(true);
        refresh();
        startTimerHz(20);
    } else {
        stopTimer();
        input_manager->setPanelOnScreen(false);
        mLevel = 0.0f;
    }
}

void AudioInputView::timerCallback()
{
    auto* input_manager = mProcessor.getAudioInputManager();

    const float peak = input_manager->hasSelectedInputDevice() ? input_manager->getAndResetPeakLevel()
                                                               : mProcessor.getAndResetHostInputPeakLevel();

    // Rise instantly, fall back slowly, so a short peak is still visible.
    mLevel = peak > mLevel ? peak : mLevel * 0.8f;

    if (mLevel < 0.001f)
        mLevel = 0.0f;

    const auto status = _getStatusText();

    if (status != mStatusText) {
        mStatusText = status;

        // The channel list only exists once the device is open, which happens asynchronously
        // enough that it can arrive after the panel was first shown.
        if (mChannelsDropDown->getNumItems() != input_manager->getInputChannelSetNames().size())
            refresh();
    }

    repaint();
}

void AudioInputView::refresh()
{
    auto* input_manager = mProcessor.getAudioInputManager();

    const ScopedValueSetter<bool> refreshing(mIsRefreshing, true);

    // Drivers
    mDriverDropDown->clear(dontSendNotification);
    const auto driver_names = input_manager->getDriverNames();
    mDriverDropDown->addItemList(driver_names, 1);

    const int driver_index = driver_names.indexOf(input_manager->getSelectedDriverName());
    mDriverDropDown->setSelectedItemIndex(jmax(driver_index, 0), dontSendNotification);

    // Inputs. The host's own audio is always the first entry, and has no device behind it.
    mInputDevices.clearQuick();
    mInputDevices.add({String(), AudioInputManager::getHostInputName()});
    mInputDevices.addArray(input_manager->getInputDevices());

    StringArray input_names;

    for (const auto& device: mInputDevices)
        input_names.add(device.name);

    mInputDropDown->clear(dontSendNotification);
    mInputDropDown->addItemList(input_names, 1);

    // Matched by id, never by what it is called: two System Audio endpoints can show the same name.
    const auto selected_id = input_manager->getSelectedInputDeviceId();
    int device_index = selected_id.isEmpty() ? 0 : -1;

    for (int i = 1; device_index < 0 && i < mInputDevices.size(); i++)
        if (mInputDevices[i].id == selected_id)
            device_index = i;

    mSelectedDeviceIsPresent = device_index >= 0;

    if (device_index < 0) {
        // The remembered device isn't there any more (unplugged, renamed, or a different machine).
        mInputDropDown->addItem(input_manager->getSelectedInputDeviceDisplayName() + " (not found)",
                                mInputDevices.size() + 1);
        mInputDropDown->setSelectedItemIndex(mInputDevices.size(), dontSendNotification);
    } else {
        mInputDropDown->setSelectedItemIndex(device_index, dontSendNotification);
    }

    // Channels
    const auto channel_names = input_manager->getInputChannelSetNames();
    mChannelsDropDown->clear(dontSendNotification);

    if (channel_names.isEmpty()) {
        mChannelsDropDown->addItem("Default", 1);
        mChannelsDropDown->setSelectedItemIndex(0, dontSendNotification);
    } else {
        mChannelsDropDown->addItemList(channel_names, 1);

        const int channel_set = jlimit(0, channel_names.size() - 1, input_manager->getSelectedInputChannelSet());

        // Pushed back into the manager, never only displayed: a device that has come back with fewer
        // inputs than when the choice was made would otherwise record a channel other than this one,
        // and clicking the entry already shown cannot put that right. Only while these really are the
        // selected device's channels, though: they are the open device's, and while some other device
        // is the open one they are a fallback's, and writing those back would overwrite a choice that
        // is still good, save it, and reopen a device over a selection that isn't there.
        if (_isSelectedDeviceOpen())
            input_manager->setSelectedInputChannelSet(channel_set);

        mChannelsDropDown->setSelectedItemIndex(channel_set, dontSendNotification);
    }

    mStatusText = _getStatusText();
    updateEnablements();
    repaint();
}

void AudioInputView::updateEnablements()
{
    auto* input_manager = mProcessor.getAudioInputManager();
    const auto state = mProcessor.getState();
    const bool is_recording = state == Recording;

    // Switching device under a running recording would leave the writers mid-file.
    mDriverDropDown->setEnabled(!is_recording);
    mInputDropDown->setEnabled(!is_recording);
    // While the selected device is away, the open one is a fallback: picking from its channels would
    // only overwrite the choice made for the device that is coming back.
    mChannelsDropDown->setEnabled(!is_recording && mSelectedDeviceIsPresent && input_manager->isInputDeviceOpen());

    mRecordButton->setEnabled(state == EmptyAudioAndMidiRegions || is_recording);
    mRecordButton->setButtonText(is_recording ? "STOP" : "RECORD");
}

void AudioInputView::_driverChanged()
{
    if (mIsRefreshing)
        return;

    mProcessor.getAudioInputManager()->setSelectedDriverName(mDriverDropDown->getText());
    refresh();
}

void AudioInputView::_inputDeviceChanged()
{
    if (mIsRefreshing)
        return;

    const int index = mInputDropDown->getSelectedItemIndex();

    // Index 0 is the host input, and anything past the real device list is the "not found" entry.
    const bool is_device = index > 0 && index < mInputDevices.size();

    mProcessor.getAudioInputManager()->setSelectedInputDevice(is_device ? mInputDevices[index]
                                                                        : AudioInputManager::InputDevice());
    refresh();
}

void AudioInputView::_channelsChanged()
{
    if (mIsRefreshing)
        return;

    mProcessor.getAudioInputManager()->setSelectedInputChannelSet(mChannelsDropDown->getSelectedItemIndex());
    refresh();
}

bool AudioInputView::_isSelectedDeviceOpen() const
{
    auto* input_manager = mProcessor.getAudioInputManager();

    // Being listed is not being the one that opened. A device can be there and still refuse to open,
    // busy in another app, and what the driver leaves open then is a different device of its own
    // choosing, whose channels have nothing to do with the selection. The manager cannot yet say
    // which device is the open one, so anything it is complaining about is taken as not ours:
    // skipping a write-back costs a reopen, making one against a fallback overwrites a good choice.
    return mSelectedDeviceIsPresent && input_manager->isInputDeviceOpen() && input_manager->getLastError().isEmpty();
}

String AudioInputView::_getStatusText() const
{
    auto* input_manager = mProcessor.getAudioInputManager();

    // Already a complete sentence, and says which way it went wrong: an input that died under a take
    // and one that came back as a different input are different sentences, not one shared one.
    const auto error = input_manager->getLastError();

    const auto state = mProcessor.getState();
    const bool take_landed = state == Processing || state == PopulatedAudioAndMidiRegions;

    StringArray sentences;
    sentences.add(error);

    // Nothing else would ever say so: a take the writers dropped blocks from has holes in it and
    // looks exactly like a whole one.
    if (take_landed && mProcessor.getSourceAudioManager()->getNumLostWriteBlocks() > 0)
        sentences.add("Some of the audio was lost while recording, so this take is incomplete.");

    // A failure is never the last word once a take has landed: record is disabled until that take is
    // cleared, and only the state message says how. The rest of the state messages describe a
    // healthy input, so a failure still stands on its own there.
    if (error.isEmpty() || take_landed)
        sentences.add(_getStateText());

    // Last, and never part of what decides any of the above: a driver reports statuses it carries on
    // through without missing a block, so this is a note for whoever is diagnosing one rather than a
    // verdict on the input, and a working device goes on reading as one. A failure of ours already
    // quotes it, so it is only added when there is none to say it twice.
    if (error.isEmpty()) {
        const auto driver_message = input_manager->getLastDriverMessage();

        // Wrapped in a sentence, never left standing on its own: the driver's wording is a fragment,
        // and often just a code.
        if (driver_message.isNotEmpty())
            sentences.add("The driver reported: " + driver_message);
    }

    sentences.removeEmptyStrings();

    return sentences.joinIntoString(" ");
}

String AudioInputView::_getStateText() const
{
    auto* input_manager = mProcessor.getAudioInputManager();
    const auto state = mProcessor.getState();

    if (state == Processing)
        return "Transcribing...";

    if (state == PopulatedAudioAndMidiRegions)
        return "Clear the current take (the bin button) before recording another.";

    if (!input_manager->hasSelectedInputDevice()) {
        if (!mCanSelectInput)
            return "Recording the audio your DAW sends to the plugin.";

        return String("Recording the audio your DAW sends to the plugin. ")
               + "To record from this computer instead, pick an input above.";
    }

    if (!input_manager->isInputDeviceOpen())
        return "Input will be opened when you record.";

    const auto sample_rate = input_manager->getCurrentSampleRate();
    const auto rate_text = String(sample_rate / 1000.0, 1) + " kHz";

    if (state == Recording)
        return "Recording from " + input_manager->getSelectedInputDeviceDisplayName() + " at " + rate_text + ".";

    return "Ready to record from " + input_manager->getSelectedInputDeviceDisplayName() + " at " + rate_text + ".";
}
