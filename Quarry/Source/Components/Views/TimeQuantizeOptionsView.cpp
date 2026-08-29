//
// Created by Damien Ronssin on 12.03.23.
//

#include "TimeQuantizeOptionsView.h"

#include <okstudio/Obsidian.h>
#include "QuarryMainView.h"

namespace
{
// The section's own content ends here; the rest is the padding the panel keeps under it.
constexpr int CONTENT_BOTTOM = LEFT_SECTIONS_TOP_PAD + 70 + 17;

static_assert(CONTENT_BOTTOM <= LeftColumnLayout::TIME_QUANTIZE_EXPANDED,
              "TIME QUANTIZE no longer fits the height the column stacks against");
} // namespace

TimeQuantizeOptionsView::TimeQuantizeOptionsView(QuarryAudioProcessor& processor)
    : mProcessor(processor)
{
    mEnableButton = std::make_unique<TextButton>("EnableTimeQuantizeOptionsButton");
    mEnableButton->setButtonText("");
    mEnableButton->setClickingTogglesState(true);
    mEnableButton->setTooltip(QuarryTooltips::tq_enable);

    mEnableButton->setColour(TextButton::buttonColourId, Colours::white.withAlpha(0.2f));

    mEnableAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(
        mProcessor.getAPVTS(), ParameterHelpers::getIdStr(ParameterHelpers::EnableTimeQuantizationId), *mEnableButton);
    addAndMakeVisible(mEnableButton.get());

    mTimeDivisionDropdown = std::make_unique<ComboBox>("TimeDivisionDropDown");
    mTimeDivisionDropdown->setEditableText(false);
    mTimeDivisionDropdown->setJustificationType(Justification::centredRight);
    mTimeDivisionDropdown->addItemList(TimeQuantizeUtils::TimeDivisionsStr, 1);
    mTimeDivisionDropdown->setTooltip(QuarryTooltips::tq_time_division);
    mTimeDivisionAttachment = std::make_unique<ComboBoxParameterAttachment>(
        *mProcessor.getParams()[ParameterHelpers::TimeDivisionId], *mTimeDivisionDropdown);
    addAndMakeVisible(*mTimeDivisionDropdown);

    mQuantizationForceSlider =
        std::make_unique<QuantizeForceSlider>(*mProcessor.getParams()[ParameterHelpers::QuantizationForceId]);
    mQuantizationForceSlider->setTooltip(QuarryTooltips::tq_quantization_force);

    addAndMakeVisible(*mQuantizationForceSlider);

    _setupTempoEditor();
    _setupTSEditors();

    mProcessor.getParams()[static_cast<size_t>(ParameterHelpers::EnableTimeQuantizationId)]->addListener(this);
    bool is_view_enabled = mProcessor.getParameterValue(ParameterHelpers::EnableTimeQuantizationId) > 0.5f;
    _setViewEnabled(is_view_enabled);
}

TimeQuantizeOptionsView::~TimeQuantizeOptionsView()
{
    mProcessor.getParams()[static_cast<size_t>(ParameterHelpers::EnableTimeQuantizationId)]->removeListener(this);
}

int TimeQuantizeOptionsView::preferredHeight() const
{
    return mIsViewEnabled ? LeftColumnLayout::TIME_QUANTIZE_EXPANDED : LeftColumnLayout::COLLAPSED;
}

void TimeQuantizeOptionsView::resized()
{
    mEnableButton->setBounds(0, 0, 18, 18);
    mTimeDivisionDropdown->setBounds(125, 13 + LEFT_SECTIONS_TOP_PAD, 75, 17);
    mQuantizationForceSlider->setBounds(94, 70 + LEFT_SECTIONS_TOP_PAD, 156, 17);
    mTempoEditor->setBounds(65, LEFT_SECTIONS_TOP_PAD + 44, 40, 14);
    mTimeSignatureNumEditor->setBounds(213, LEFT_SECTIONS_TOP_PAD + 44, 20, 14);
    mTimeSignatureDenomEditor->setBounds(238, LEFT_SECTIONS_TOP_PAD + 44, 20, 14);
}

void TimeQuantizeOptionsView::paint(Graphics& g)
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

    float alpha = isEnabled() && mIsViewEnabled ? 1.0f : DISABLED_ALPHA;

    g.setColour(TEXT_MAIN.withAlpha(alpha));
    g.setFont(UIDefines::TITLE_FONT());
    g.drawText("TIME QUANTIZE", Rectangle<int>(24, 0, 210, 17), Justification::centredLeft);

    // Collapsed, there is nothing under the label row for these to name.
    if (!mIsViewEnabled) {
        return;
    }

    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("TEMPO", Rectangle<int>(19, LEFT_SECTIONS_TOP_PAD + 45, 50, 14), Justification::centredLeft);

    g.drawText("TIME SIGNATURE", Rectangle<int>(110, LEFT_SECTIONS_TOP_PAD + 45, 100, 14), Justification::centredRight);
    g.drawText("/", Rectangle<int>(233, LEFT_SECTIONS_TOP_PAD + 45, 5, 14), Justification::centred);

    g.drawText("TIME DIVISION", Rectangle<int>(19, mTimeDivisionDropdown->getY(), 120, 17), Justification::centredLeft);

    g.drawText("FORCE", Rectangle<int>(19, mQuantizationForceSlider->getY(), 37, 17), Justification::centredLeft);
}

void TimeQuantizeOptionsView::parameterValueChanged(int parameterIndex, float newValue)
{
    if (parameterIndex != static_cast<int>(ParameterHelpers::EnableTimeQuantizationId)) {
        return;
    }

    // Marshalled for the same reason as NoteOptionsView's: this arrives on whichever thread
    // moved the parameter, and it now moves the whole left column, not just this panel.
    MessageManager::callAsync(
        [safe = Component::SafePointer<TimeQuantizeOptionsView>(this), enable = newValue > 0.5f] {
            if (safe != nullptr) {
                safe->_setViewEnabled(enable);
            }
        });
}

void TimeQuantizeOptionsView::parameterGestureChanged(int parameterIndex, bool gestureIsStarting)
{
    ignoreUnused(parameterIndex, gestureIsStarting);
}

void TimeQuantizeOptionsView::_setViewEnabled(bool inEnable)
{
    const bool changed = mIsViewEnabled != inEnable;

    mIsViewEnabled = inEnable;

    // Hidden as well as disabled: collapsed, the panel is one label row tall and there is no
    // room under it for anything to be drawn in.
    mQuantizationForceSlider->setVisible(inEnable);
    mTimeDivisionDropdown->setVisible(inEnable);
    mTempoEditor->setVisible(inEnable);
    mTimeSignatureNumEditor->setVisible(inEnable);
    mTimeSignatureDenomEditor->setVisible(inEnable);

    mQuantizationForceSlider->setEnabled(inEnable);
    mTimeDivisionDropdown->setEnabled(inEnable);

    // Obsidian dims the dropdown and the slider it draws, but a plain TextEditor keeps
    // its colours whatever setEnabled says, so these three would read as live in a dead
    // row. Nothing else dims them, so this is not the double-dimming removed elsewhere.
    auto alpha = inEnable ? 1.0f : DISABLED_ALPHA;
    mTempoEditor->setAlpha(alpha);
    mTimeSignatureNumEditor->setAlpha(alpha);
    mTimeSignatureDenomEditor->setAlpha(alpha);
    mTempoEditor->setEnabled(inEnable);
    mTimeSignatureNumEditor->setEnabled(inEnable);
    mTimeSignatureDenomEditor->setEnabled(inEnable);

    repaint();

    if (changed && onPreferredHeightChanged != nullptr) {
        onPreferredHeightChanged();
    }
}

void TimeQuantizeOptionsView::_setupTempoEditor()
{
    auto tempo_str_validator = [](const String& tempo_str) {
        if (tempo_str.isEmpty()) {
            return false;
        }

        float tempo = tempo_str.getFloatValue();
        return tempo >= 20.0f && tempo <= 999.0f;
    };

    auto tempo_str_corrector = [](const String& tempo_str) {
        return tempo_str.isEmpty() ? String("120") : String(jlimit(20.0f, 999.0f, tempo_str.getFloatValue()));
    };

    mTempoEditor = std::make_unique<NumericTextEditor<double>>(
        &mProcessor, NnId::TempoId, 6, 120.0, Justification::centredLeft, tempo_str_validator, tempo_str_corrector);

    mTempoEditor->setTooltip(QuarryTooltips::tq_tempo);
    addAndMakeVisible(mTempoEditor.get());
}

void TimeQuantizeOptionsView::_setupTSEditors()
{
    // Numerator
    auto numerator_validator = [](const String& num_str) {
        if (num_str.isEmpty()) {
            return false;
        }

        int num_val = num_str.getIntValue();
        return num_val >= 1 && num_val <= 96;
    };

    auto numerator_corrector = [](const String& num_str) {
        return num_str.isEmpty() ? String("4") : String(jlimit(1, 96, num_str.getIntValue()));
    };

    mTimeSignatureNumEditor = std::make_unique<NumericTextEditor<int>>(&mProcessor,
                                                                       NnId::TimeSignatureNumeratorId,
                                                                       2,
                                                                       4,
                                                                       Justification::centredRight,
                                                                       numerator_validator,
                                                                       numerator_corrector);
    mTimeSignatureNumEditor->setTooltip(QuarryTooltips::tq_numerator);

    addAndMakeVisible(mTimeSignatureNumEditor.get());

    // Denominator
    auto denominator_validator = [](const String& denom_str) {
        if (denom_str.isEmpty()) {
            return false;
        }

        int denom_val = denom_str.getIntValue();

        // Closest power of 2
        int new_denom_val = static_cast<int>(std::exp2(std::round(std::log2(denom_val))));

        return denom_val == new_denom_val && denom_val >= 1 && denom_val <= 64;
    };

    auto denominator_corrector = [](const String& denom_str) {
        if (denom_str.isEmpty()) {
            return String("4");
        }

        auto denom_val = denom_str.getIntValue();
        auto new_denom_val = static_cast<int>(std::exp2(std::round(std::log2(denom_val))));

        return String(jlimit(1, 64, new_denom_val));
    };

    mTimeSignatureDenomEditor = std::make_unique<NumericTextEditor<int>>(&mProcessor,
                                                                         NnId::TimeSignatureDenominatorId,
                                                                         2,
                                                                         4,
                                                                         Justification::centredLeft,
                                                                         denominator_validator,
                                                                         denominator_corrector);
    mTimeSignatureDenomEditor->setTooltip(QuarryTooltips::tq_denominator);

    addAndMakeVisible(mTimeSignatureDenomEditor.get());
}