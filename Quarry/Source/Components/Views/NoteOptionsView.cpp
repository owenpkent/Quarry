//
// Created by Damien Ronssin on 12.03.23.
//

#include "NoteOptionsView.h"

#include <okstudio/Obsidian.h>
#include "QuarryMainView.h"

NoteOptionsView::NoteOptionsView(QuarryAudioProcessor& processor)
    : mProcessor(processor)
{
    mEnableButton = std::make_unique<TextButton>("EnableNoteOptionsButton");
    mEnableButton->setButtonText("");
    mEnableButton->setClickingTogglesState(true);

    mEnableButton->setColour(TextButton::buttonColourId, Colours::white.withAlpha(0.2f));
    mEnableButton->setTooltip(QuarryTooltips::sq_enable);

    mEnableAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(
        mProcessor.getAPVTS(), ParameterHelpers::getIdStr(ParameterHelpers::EnableNoteQuantizationId), *mEnableButton);
    addAndMakeVisible(mEnableButton.get());

    mMinMaxNoteSlider = std::make_unique<MinMaxNoteSlider>(*mProcessor.getParams()[ParameterHelpers::MinMidiNoteId],
                                                           *mProcessor.getParams()[ParameterHelpers::MaxMidiNoteId]);
    mMinMaxNoteSlider->setTooltip(QuarryTooltips::sq_note_range);
    addAndMakeVisible(mMinMaxNoteSlider.get());

    mRootNoteDropdown = std::make_unique<ComboBox>("KeyRootNoteDropDown");
    mRootNoteDropdown->setEditableText(false);
    mRootNoteDropdown->setJustificationType(Justification::centredLeft);
    mRootNoteDropdown->addItemList(NoteUtils::RootNotesSharpStr, 1);
    mRootNoteDropdown->setTooltip(QuarryTooltips::sq_root_note);
    mKeyAttachment = std::make_unique<ComboBoxParameterAttachment>(
        *mProcessor.getParams()[ParameterHelpers::KeyRootNoteId], *mRootNoteDropdown);
    addAndMakeVisible(mRootNoteDropdown.get());

    mKeyType = std::make_unique<ComboBox>("ScaleTypeDropDown");
    mKeyType->setEditableText(false);
    mKeyType->setJustificationType(Justification::centredLeft);
    mKeyType->addItemList(NoteUtils::ScaleTypesStr, 1);
    mKeyType->setTooltip(QuarryTooltips::sq_scale_type);
    mKeyTypeAttachment =
        std::make_unique<ComboBoxParameterAttachment>(*mProcessor.getParams()[ParameterHelpers::KeyTypeId], *mKeyType);
    addAndMakeVisible(mKeyType.get());

    mSnapMode = std::make_unique<ComboBox>("SnapModeDropDown");
    mSnapMode->setEditableText(false);
    mSnapMode->setJustificationType(Justification::centredLeft);
    mSnapMode->addItemList(NoteUtils::SnapModesStr, 1);
    mSnapMode->setTooltip(QuarryTooltips::sq_snap_mode);
    mSnapModeAttachment = std::make_unique<ComboBoxParameterAttachment>(
        *mProcessor.getParams()[ParameterHelpers::KeySnapModeId], *mSnapMode);
    addAndMakeVisible(mSnapMode.get());

    // A reading of the take, not an instruction to it. Sits in a well beside the
    // snap controls so output does not look like something you set.
    mDetectedLabel = std::make_unique<Label>("DetectedKey");
    mDetectedLabel->setJustificationType(Justification::centredLeft);
    mDetectedLabel->setInterceptsMouseClicks(false, false);
    mDetectedLabel->setTooltip(QuarryTooltips::detected_key);
    addAndMakeVisible(*mDetectedLabel);

    mUseKeyButton = std::make_unique<TextButton>("Use it");
    mUseKeyButton->setTooltip(QuarryTooltips::use_detected_key);
    mUseKeyButton->onClick = [this]() { _adoptDetectedKey(); };
    addAndMakeVisible(*mUseKeyButton);

    setSize(266, 169);

    _refreshDetectedKey();
    startTimerHz(2);

    mProcessor.getParams()[static_cast<size_t>(ParameterHelpers::EnableNoteQuantizationId)]->addListener(this);

    bool is_enabled = mProcessor.getParameterValue(ParameterHelpers::EnableNoteQuantizationId) > 0.5f;
    _enableView(is_enabled);
}

NoteOptionsView::~NoteOptionsView()
{
    stopTimer();
    mProcessor.getParams()[static_cast<size_t>(ParameterHelpers::EnableNoteQuantizationId)]->removeListener(this);
}

void NoteOptionsView::resized()
{
    mEnableButton->setBounds(0, 0, 18, 18);
    mMinMaxNoteSlider->setBounds(64, 17 + LEFT_SECTIONS_TOP_PAD, 189, 17);
    mRootNoteDropdown->setBounds(64, LEFT_SECTIONS_TOP_PAD + 46, 55, 17);
    mKeyType->setBounds(124, LEFT_SECTIONS_TOP_PAD + 46, 129, 17);
    mSnapMode->setBounds(100, LEFT_SECTIONS_TOP_PAD + 75, 154, 17);
    mDetectedLabel->setBounds(83, LEFT_SECTIONS_TOP_PAD + 104, 116, 17);
    mUseKeyButton->setBounds(203, LEFT_SECTIONS_TOP_PAD + 103, 50, 19);
}

void NoteOptionsView::paint(Graphics& g)
{
    okstudio::obsidian::raisedFill(g,
                                   Rectangle<float>(0.0f,
                                                    static_cast<float>(LEFT_SECTIONS_TOP_PAD),
                                                    static_cast<float>(getWidth()),
                                                    static_cast<float>(getHeight() - LEFT_SECTIONS_TOP_PAD)),
                                   5.0f,
                                   PANEL_TOP,
                                   PANEL_BOT);

    float alpha = mIsViewEnabled && isEnabled() ? 1.0f : DISABLED_ALPHA;

    g.setColour(TEXT_MAIN.withAlpha(alpha));

    g.setFont(UIDefines::TITLE_FONT());
    g.drawText("SCALE QUANTIZE", Rectangle<int>(24, 0, 274, 17), Justification::centredLeft);

    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("RANGE", Rectangle<int>(19, mMinMaxNoteSlider->getY(), 80, 17), Justification::centredLeft);

    g.drawText("KEY", Rectangle<int>(19, mRootNoteDropdown->getY(), 80, 17), Justification::centredLeft);

    g.drawText("SNAP MODE", Rectangle<int>(19, mSnapMode->getY(), 80, 17), Justification::centredLeft);

    // Detected is not part of the disabled group: it describes the take, which is
    // true whether or not snapping is switched on.
    g.setColour(TEXT_MAIN);
    g.drawText("DETECTED", Rectangle<int>(19, mDetectedLabel->getY(), 80, 17), Justification::centredLeft);
}

void NoteOptionsView::parameterValueChanged(int parameterIndex, float newValue)
{
    if (parameterIndex == static_cast<int>(ParameterHelpers::EnableNoteQuantizationId)) {
        bool enable = newValue > 0.5f;
        _enableView(enable);
    }
}

void NoteOptionsView::parameterGestureChanged(int parameterIndex, bool gestureIsStarting)
{
    ignoreUnused(parameterIndex, gestureIsStarting);
}

void NoteOptionsView::_enableView(bool inEnable)
{
    mIsViewEnabled = inEnable;
    mMinMaxNoteSlider->setEnabled(inEnable);
    mRootNoteDropdown->setEnabled(inEnable);
    mKeyType->setEnabled(inEnable);
    mSnapMode->setEnabled(inEnable);
    repaint();
}

void NoteOptionsView::timerCallback()
{
    _refreshDetectedKey();
}

void NoteOptionsView::_refreshDetectedKey()
{
    const auto& events = mProcessor.getTranscriptionManager()->getNoteEventVector();

    if (events.size() == mLastNoteCount)
        return;

    mLastNoteCount = events.size();
    mDetected = estimateKey(events);

    const bool usable = mDetected.isValid();

    if (!usable) {
        mDetectedLabel->setColour(Label::textColourId, TEXT_FAINT);
        mDetectedLabel->setText(events.empty() ? "nothing yet" : "no clear key", dontSendNotification);
    } else {
        // Confidence is shown rather than hidden: percussive material scores low,
        // and the user should be able to see that before trusting the answer.
        mDetectedLabel->setColour(Label::textColourId, okstudio::obsidian::accentOf(*this).base);
        mDetectedLabel->setText(mDetected.toString() + "  " + String(mDetected.confidence, 2),
                                dontSendNotification);
    }

    mUseKeyButton->setEnabled(usable);
}

void NoteOptionsView::_adoptDetectedKey()
{
    if (!mDetected.isValid())
        return;

    // The estimator counts from C; the picker starts at A.
    const int root_index = (mDetected.rootNote + 3) % 12;

    mRootNoteDropdown->setSelectedItemIndex(root_index, sendNotificationSync);
    mKeyType->setSelectedItemIndex(mDetected.isMinor ? NoteUtils::Minor : NoteUtils::Major,
                                   sendNotificationSync);
}
