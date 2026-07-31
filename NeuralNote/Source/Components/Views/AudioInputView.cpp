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

// The primary target of the whole app, so it is sized to be hit with a mouse without aiming.
constexpr int kRecordButtonWidth = 180;
constexpr int kRecordButtonHeight = 72;
} // namespace

AudioInputView::AudioInputView(NeuralNoteAudioProcessor& inProcessor, std::function<void()> inOnRecordClicked)
    : mProcessor(inProcessor)
    , mOnRecordClicked(std::move(inOnRecordClicked))
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

    mDriverDropDown = make_drop_down("Driver", NeuralNoteTooltips::ai_driver, [this] { _driverChanged(); });
    mInputDropDown = make_drop_down("Input", NeuralNoteTooltips::ai_device, [this] { _inputDeviceChanged(); });
    mChannelsDropDown = make_drop_down("Channels", NeuralNoteTooltips::ai_channels, [this] { _channelsChanged(); });

    mRecordButton = std::make_unique<TextButton>("RECORD");
    mRecordButton->setTooltip(NeuralNoteTooltips::record);
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
    g.drawText("DRIVER", Rectangle<int>(kLabelX, kDriverRowY, kLabelWidth, kRowHeight), Justification::centredLeft);
    g.drawText("INPUT", Rectangle<int>(kLabelX, kInputRowY, kLabelWidth, kRowHeight), Justification::centredLeft);
    g.drawText("CHANNELS", Rectangle<int>(kLabelX, kChannelsRowY, kLabelWidth, kRowHeight), Justification::centredLeft);
    g.drawText("LEVEL", Rectangle<int>(kLabelX, kMeterY, kLabelWidth, kMeterHeight), Justification::centredLeft);

    // Level meter
    const int meter_width = getWidth() - kControlX - kLabelX;
    auto meter_bounds = Rectangle<int>(kControlX, kMeterY, meter_width, kMeterHeight).toFloat();

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
                     Rectangle<int>(kLabelX, kStatusY, status_width, getHeight() - kStatusY - 10),
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

    // Inputs. The host's own audio is always the first entry.
    mInputDeviceNames.clear();
    mInputDeviceNames.add(AudioInputManager::getHostInputName());
    mInputDeviceNames.addArray(input_manager->getInputDeviceNames());

    mInputDropDown->clear(dontSendNotification);
    mInputDropDown->addItemList(mInputDeviceNames, 1);

    const auto selected_device = input_manager->getSelectedInputDeviceName();
    const int device_index = selected_device.isEmpty() ? 0 : mInputDeviceNames.indexOf(selected_device);

    if (device_index < 0) {
        // The remembered device isn't there any more (unplugged, or a different machine).
        mInputDropDown->addItem(selected_device + " (not found)", mInputDeviceNames.size() + 1);
        mInputDropDown->setSelectedItemIndex(mInputDeviceNames.size(), dontSendNotification);
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
        mChannelsDropDown->setSelectedItemIndex(
            jlimit(0, channel_names.size() - 1, input_manager->getSelectedInputChannelSet()), dontSendNotification);
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
    mChannelsDropDown->setEnabled(!is_recording && input_manager->isInputDeviceOpen());

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
    const bool is_device = index > 0 && index < mInputDeviceNames.size();

    mProcessor.getAudioInputManager()->setSelectedInputDeviceName(is_device ? mInputDeviceNames[index] : String());
    refresh();
}

void AudioInputView::_channelsChanged()
{
    if (mIsRefreshing)
        return;

    mProcessor.getAudioInputManager()->setSelectedInputChannelSet(mChannelsDropDown->getSelectedItemIndex());
    refresh();
}

String AudioInputView::_getStatusText() const
{
    auto* input_manager = mProcessor.getAudioInputManager();

    const auto error = input_manager->getLastError();

    if (error.isNotEmpty())
        return "Could not open this input: " + error;

    const auto state = mProcessor.getState();

    if (state == Processing)
        return "Transcribing...";

    if (state == PopulatedAudioAndMidiRegions)
        return "Clear the current take (the bin button) before recording another.";

    if (!input_manager->hasSelectedInputDevice()) {
        return String("Recording the audio your DAW sends to the plugin. ")
               + "To record from this computer instead, pick an input above.";
    }

    if (!input_manager->isInputDeviceOpen())
        return "Input will be opened when you record.";

    const auto sample_rate = input_manager->getCurrentSampleRate();
    const auto rate_text = String(sample_rate / 1000.0, 1) + " kHz";

    if (state == Recording)
        return "Recording from " + input_manager->getSelectedInputDeviceName() + " at " + rate_text + ".";

    return "Ready to record from " + input_manager->getSelectedInputDeviceName() + " at " + rate_text + ".";
}
