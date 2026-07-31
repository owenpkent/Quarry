//
// Created by Damien Ronssin on 17.03.23.
//

#ifndef QuarryLNF_h
#define QuarryLNF_h

#include "JuceHeader.h"
#include "UIDefines.h"

class QuarryLNF : public juce::LookAndFeel_V4
{
public:
    QuarryLNF();

    Font getComboBoxFont(ComboBox& /*box*/) override { return UIDefines::LABEL_FONT(); }

    Font getPopupMenuFont() override { return UIDefines::LABEL_FONT(); }

    Font getTextButtonFont(TextButton&, int buttonHeight) override { return UIDefines::LARGE_FONT(); };

    Font getLabelFont(juce::Label&) override { return UIDefines::DROPDOWN_FONT(); };

    void drawRotarySlider(Graphics&,
                          int x,
                          int y,
                          int width,
                          int height,
                          float sliderPosProportional,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          Slider&) override;

    void drawTooltip(Graphics&, const String&, int, int) override;
};

#endif // QuarryLNF_h
