//
// Created by Damien Ronssin on 20.03.23.
//

#include "QuantizeForceSlider.h"

QuantizeForceSlider::QuantizeForceSlider(RangedAudioParameter& inAttachedValue)
{
    mSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    mSlider.setRange(0, 100, 1);
    mSlider.setTextBoxStyle(juce::Slider::TextEntryBoxPosition::NoTextBox, true, 0, 0);
    mSlider.onValueChange = [this]() { repaint(); };

    mAttachment = std::make_unique<juce::SliderParameterAttachment>(inAttachedValue, mSlider);

    mSlider.onValueChange = [this]() { repaint(); };

    addAndMakeVisible(mSlider);
}

void QuantizeForceSlider::resized()
{
    mSlider.setBounds(0, 0, 128, 17);
}

void QuantizeForceSlider::paint(Graphics& g)
{
    // Painted here rather than by the look and feel, so nothing else fades it when the row
    // is switched off. Same reason as the note names in MinMaxNoteSlider.
    g.setColour(TEXT_MAIN.withMultipliedAlpha(isEnabled() ? 1.0f : DISABLED_ALPHA));
    g.setFont(UIDefines::DROPDOWN_FONT());

    g.drawText(std::to_string(static_cast<int>(std::round(mSlider.getValue() * 100.0f))),
               Rectangle<int>(133, 0, 23, 17),
               juce::Justification::centredRight);
}

void QuantizeForceSlider::setTooltip(const String& inTooltip)
{
    mSlider.setTooltip(inTooltip);
}
