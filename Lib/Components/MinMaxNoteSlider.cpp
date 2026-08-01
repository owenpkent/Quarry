//
// Created by Damien Ronssin on 14.03.23.
//

#include "MinMaxNoteSlider.h"

MinMaxNoteSlider::MinMaxNoteSlider(RangedAudioParameter& inMinValue, RangedAudioParameter& inMaxValue)

{
    mSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    mSlider.setRange(MIN_MIDI_NOTE, MAX_MIDI_NOTE, 1);
    mSlider.setTextBoxStyle(juce::Slider::TextEntryBoxPosition::NoTextBox, true, 0, 0);
    mAttachment = std::make_unique<TwoValueAttachment>(mSlider, inMinValue, inMaxValue);

    mSlider.onValueChange = [this] { repaint(); };

    addAndMakeVisible(mSlider);
}

void MinMaxNoteSlider::resized()
{
    mSlider.setBounds(17, 0, 156, 17);
}

void MinMaxNoteSlider::paint(Graphics& g)
{
    // These two names are painted here rather than by the look and feel, so nothing else
    // fades them when the row is switched off and they would sit at full strength beside
    // captions that have already gone quiet.
    g.setColour(TEXT_MAIN.withMultipliedAlpha(isEnabled() ? 1.0f : DISABLED_ALPHA));
    g.setFont(UIDefines::DROPDOWN_FONT());

    g.drawText(NoteUtils::midiNoteToStr(int(mSlider.getMinValue())),
               Rectangle<int>(0, 0, 22, 12),
               juce::Justification::centredLeft);

    g.drawText(NoteUtils::midiNoteToStr(int(mSlider.getMaxValue())),
               Rectangle<int>(168, 0, 22, 12),
               juce::Justification::centredRight);
}

void MinMaxNoteSlider::setTooltip(const String& inTooltip)
{
    mSlider.setTooltip(inTooltip);
}
