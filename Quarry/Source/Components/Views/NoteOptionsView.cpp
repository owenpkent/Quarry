//
// Created by Damien Ronssin on 12.03.23.
//

#include "NoteOptionsView.h"

#include <okstudio/Obsidian.h>
#include "QuarryMainView.h"

namespace
{
// The section's own content ends here; the rest is the padding the panel keeps under it.
constexpr int CONTENT_BOTTOM = LEFT_SECTIONS_TOP_PAD + 75 + 17;

static_assert(CONTENT_BOTTOM <= LeftColumnLayout::SCALE_QUANTIZE_EXPANDED,
              "SCALE QUANTIZE no longer fits the height the column stacks against");
static_assert(LeftColumnLayout::COLLAPSED == LEFT_SECTIONS_TOP_PAD,
              "a collapsed section is its label row, which is what LEFT_SECTIONS_TOP_PAD reserves");
} // namespace

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

    // The detected key used to be reported here, beside the snap controls that act on it. It
    // is in TranscriptionSummary now, with the tempo and the meter and everything else that
    // describes the take rather than instructs it, and the button that adopts it went along.
    // Two readouts of one number is one of them being out of date.

    setSize(266, 140);

    mProcessor.getParams()[static_cast<size_t>(ParameterHelpers::EnableNoteQuantizationId)]->addListener(this);

    bool is_enabled = mProcessor.getParameterValue(ParameterHelpers::EnableNoteQuantizationId) > 0.5f;
    _enableView(is_enabled);
}

NoteOptionsView::~NoteOptionsView()
{
    mProcessor.getParams()[static_cast<size_t>(ParameterHelpers::EnableNoteQuantizationId)]->removeListener(this);
}

int NoteOptionsView::preferredHeight() const
{
    return mIsViewEnabled ? LeftColumnLayout::SCALE_QUANTIZE_EXPANDED : LeftColumnLayout::COLLAPSED;
}

void NoteOptionsView::resized()
{
    mEnableButton->setBounds(0, 0, 18, 18);
    mMinMaxNoteSlider->setBounds(64, 17 + LEFT_SECTIONS_TOP_PAD, 189, 17);
    mRootNoteDropdown->setBounds(64, LEFT_SECTIONS_TOP_PAD + 46, 55, 17);
    mKeyType->setBounds(124, LEFT_SECTIONS_TOP_PAD + 46, 129, 17);
    mSnapMode->setBounds(100, LEFT_SECTIONS_TOP_PAD + 75, 154, 17);
}

void NoteOptionsView::paint(Graphics& g)
{
    if (mIsViewEnabled) {
        okstudio::obsidian::raisedFill(
            g,
            Rectangle<float>(0.0f,
                             static_cast<float>(LEFT_SECTIONS_TOP_PAD),
                             static_cast<float>(getWidth()),
                             static_cast<float>(getHeight() - LEFT_SECTIONS_TOP_PAD)),
            5.0f,
            PANEL_TOP,
            PANEL_BOT);
    }

    float alpha = mIsViewEnabled && isEnabled() ? 1.0f : DISABLED_ALPHA;

    g.setColour(TEXT_MAIN.withAlpha(alpha));

    g.setFont(UIDefines::TITLE_FONT());
    g.drawText("SCALE QUANTIZE", Rectangle<int>(24, 0, 274, 17), Justification::centredLeft);

    // Nothing below the label row exists when the section is collapsed, so neither do the words
    // naming it.
    if (!mIsViewEnabled) {
        return;
    }

    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("RANGE", Rectangle<int>(19, mMinMaxNoteSlider->getY(), 80, 17), Justification::centredLeft);

    g.drawText("KEY", Rectangle<int>(19, mRootNoteDropdown->getY(), 80, 17), Justification::centredLeft);

    g.drawText("SNAP MODE", Rectangle<int>(19, mSnapMode->getY(), 80, 17), Justification::centredLeft);
}

void NoteOptionsView::parameterValueChanged(int parameterIndex, float newValue)
{
    if (parameterIndex != static_cast<int>(ParameterHelpers::EnableNoteQuantizationId)) {
        return;
    }

    // Delivered on whichever thread moved the parameter, which for an automated one is the audio
    // thread, and everything _enableView touches is a component. It used to call straight
    // through; now that it also moves the section's height, and with it the whole left column,
    // getting that wrong is a good deal louder than a stale repaint.
    MessageManager::callAsync(
        [safe = Component::SafePointer<NoteOptionsView>(this), enable = newValue > 0.5f] {
            if (safe != nullptr) {
                safe->_enableView(enable);
            }
        });
}

void NoteOptionsView::parameterGestureChanged(int parameterIndex, bool gestureIsStarting)
{
    ignoreUnused(parameterIndex, gestureIsStarting);
}

void NoteOptionsView::_enableView(bool inEnable)
{
    const bool changed = mIsViewEnabled != inEnable;

    mIsViewEnabled = inEnable;

    // Hidden as well as disabled: below the label row there is no room for them at all when the
    // section is collapsed, and a control drawn outside its panel's bounds would spill onto
    // whatever the column stacked underneath.
    mMinMaxNoteSlider->setVisible(inEnable);
    mRootNoteDropdown->setVisible(inEnable);
    mKeyType->setVisible(inEnable);
    mSnapMode->setVisible(inEnable);

    mMinMaxNoteSlider->setEnabled(inEnable);
    mRootNoteDropdown->setEnabled(inEnable);
    mKeyType->setEnabled(inEnable);
    mSnapMode->setEnabled(inEnable);

    repaint();

    if (changed && onPreferredHeightChanged != nullptr) {
        onPreferredHeightChanged();
    }
}

