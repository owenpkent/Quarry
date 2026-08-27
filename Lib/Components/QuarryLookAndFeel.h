//
// Quarry's look and feel: Obsidian, plus the two things Obsidian does not draw.
//
// This is a subclass rather than an edit to okstudio/Obsidian.h on purpose. That header
// is vendored from the private kit repo and re-copied by tools/sync_okstudio.py, so any
// change made there is drift that the next sync silently reverts. Anything Quarry needs
// and the line has not adopted belongs here. If one of these becomes right for every
// product, it moves upstream and comes back through the sync.
//
// What it adds, both from docs/UI_AUDIT.md:
//
//   A boundary. CONTROL_BG and PANEL_TOP are the same value, so an Obsidian button drawn
//   on a panel is separated from it by 1.00:1 and a 1px catch-light. Controls get a
//   CONTROL_BORDER outline, which is the 3:1 SC 1.4.11 asks for without a light grey
//   button, since the fill stays dark.
//
//   A focus ring. Obsidian draws none, on anything, so nothing in the product has ever
//   shown keyboard focus. Cheap to add and free on a control that never gets focused,
//   which is most of them while the mouse-only question in UI_AUDIT.md section 5 is open.
//
// Roles ride on Component::getProperties() rather than a subclass so that a plain
// juce::TextButton can carry one. See quarry::lnf::setRole.
//

#ifndef QUARRY_LOOKANDFEEL_H
#define QUARRY_LOOKANDFEEL_H

#include <okstudio/Obsidian.h>

#include "UIDefines.h"

namespace quarry::lnf
{
// A button's job, which decides its fill, its border and its text. Obsidian has one
// button; the product has four kinds of action and no way to tell them apart.
enum class Role
{
    secondary,   // the default: dark fill, CONTROL_BORDER outline
    primary,     // accent fill, dark text. At most one per screen.
    quiet,       // no fill, no border. Only next to something bordered.
    destructive  // discards user work
};

// #ff6b7f rather than RECORD_RED: the record light is a graphic and 3.71:1 is fine for
// one, but a destructive button's border and label are read, so they answer to 4.5:1.
// This is 5.60:1 on a panel and 5.26:1 on a control, and keeps the hue.
static const juce::Colour DESTRUCTIVE(static_cast<juce::uint8>(0xff),
                                      static_cast<juce::uint8>(0x6b),
                                      static_cast<juce::uint8>(0x7f));

// Disabled: a solid pair rather than Obsidian's beginTransparencyLayer(0.45f), which
// lands the boundary at 1.09:1 and the label at 1.99:1 and drops the control out of the
// interface entirely. docs/ACCESSIBILITY.md 1.4 wants 1.5:1 and 2.5:1.
// 1.63:1 on a control and 1.74:1 on a panel. The first value tried here was #3d4149,
// which looked right and measured 1.41:1; tools/contrast_check.py is what caught it.
static const juce::Colour DISABLED_BORDER(static_cast<juce::uint8>(0x47),
                                          static_cast<juce::uint8>(0x4a),
                                          static_cast<juce::uint8>(0x54));
static const juce::Colour DISABLED_TEXT(static_cast<juce::uint8>(0x6a),
                                        static_cast<juce::uint8>(0x70),
                                        static_cast<juce::uint8>(0x7a));

inline const juce::Identifier& rolePropertyId()
{
    static const juce::Identifier id("quarryRole");
    return id;
}

// Tag a button with its role. Call after construction; the look and feel reads it back
// at paint time, so it can be changed later (the record button flips to primary when it
// arms) without rebuilding the control.
inline void setRole(juce::Component& c, Role r)
{
    c.getProperties().set(rolePropertyId(), static_cast<int>(r));
}

inline Role roleOf(const juce::Component& c)
{
    const auto v = c.getProperties().getWithDefault(rolePropertyId(), static_cast<int>(Role::secondary));
    return static_cast<Role>(static_cast<int>(v));
}

// The focus ring: 2px of accent just outside the control, per docs/ACCESSIBILITY.md 3.
// Drawn outside rather than inside so it never eats the control's own border, which is
// what identifies it; the two would otherwise fight for the same pixel.
//
// Layout has to leave room for this. Where a parent clips it, the ring is still drawn,
// just cropped, which is why the standard asks for 2px of padding around controls.
inline void focusRing(juce::Graphics& g, juce::Rectangle<float> r, float cornerRadius,
                      juce::Colour accent)
{
    g.setColour(accent);
    g.drawRoundedRectangle(r.expanded(2.0f), cornerRadius + 2.0f, 2.0f);
}

// hasKeyboardFocus(false): this component itself, not a child. JUCE has no
// :focus-visible equivalent, so a control focused by a click shows the ring too. That is
// the safe direction to err in - a ring that appears once on click is noise, a ring that
// never appears is SC 2.4.7.
inline bool shouldShowFocus(const juce::Component& c)
{
    return c.isEnabled() && c.hasKeyboardFocus(false);
}

class LookAndFeel : public okstudio::obsidian::LookAndFeel
{
public:
    LookAndFeel() = default;

    // Reimplemented rather than delegating: the border, the role fills and the state
    // handling all change, and the base would have to be undone more than reused. It
    // still leans on Obsidian's own primitives so the chip keeps the family look.
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool highlighted, bool down) override
    {
        using namespace juce;
        namespace ob = okstudio::obsidian;

        const auto r = button.getLocalBounds().toFloat().reduced(1.0f);
        const bool enabled = button.isEnabled();
        const auto accent = ob::accentOf(button);
        const auto role = roleOf(button);

        // A toggled button reads as lit whatever its role, which is the one state that
        // carries information rather than feedback and so owes 3:1 against its off state.
        const bool lit = button.getToggleState() || role == Role::primary;

        Colour fill, border;
        if (! enabled)
        {
            fill = CONTROL_BG;
            border = DISABLED_BORDER;
        }
        else if (lit)
        {
            fill = accent.base;
            border = accent.base;
        }
        else
        {
            switch (role)
            {
                case Role::destructive: fill = CONTROL_BG; border = DESTRUCTIVE; break;
                case Role::quiet:       fill = Colours::transparentBlack; border = Colours::transparentBlack; break;
                case Role::primary:     fill = accent.base; border = accent.base; break;
                case Role::secondary:
                default:                fill = backgroundColour.isTransparent() ? CONTROL_BG : backgroundColour;
                                        border = CONTROL_BORDER; break;
            }
        }

        // Hover is feedback, not information, so it is not held to 3:1 (see
        // docs/ACCESSIBILITY.md 1.3). It does have to be obvious, and Obsidian's
        // brighter(0.12f) on the fill alone is 1.40:1, which is not. Brightening the
        // border as well is what actually reads, and costs nothing on the panel.
        if (enabled && highlighted && ! down)
        {
            fill = fill.brighter(0.18f);
            if (! border.isTransparent())
                border = lit ? accent.hot : TEXT_MAIN;
        }

        // Pressed cannot be signalled by darkening: the rest surface is already near the
        // floor, so no darker value reaches even 1.5:1. Seat the chip instead by dropping
        // the catch-light, which is a shape change and survives any display.
        const bool seated = enabled && down;

        if (! fill.isTransparent())
        {
            ob::raisedFill(g, r, ob::radius,
                           fill.brighter(seated ? 0.0f : 0.05f),
                           fill.darker(seated ? 0.02f : 0.16f),
                           ! seated);
        }

        if (! border.isTransparent())
        {
            g.setColour(border);
            g.drawRoundedRectangle(r, ob::radius, seated ? 1.4f : 1.0f);
        }

        if (enabled && lit)
            ob::glowRect(g, r, ob::radius, accent.base, 0.9f);

        if (shouldShowFocus(button))
            focusRing(g, r, ob::radius, accent.base);
    }

    // Obsidian resolves button text from textColourOffId/textColourOnId, which callers
    // set per button. The roles need to override that: a primary button's dark-on-accent
    // is not something the caller should have to remember, and getting it wrong is the
    // white-on-cyan 2.09:1 case.
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool highlighted, bool down) override
    {
        using namespace juce;
        namespace ob = okstudio::obsidian;

        const auto role = roleOf(button);
        const bool lit = button.getToggleState() || role == Role::primary;

        Colour c;
        if (! button.isEnabled())
            c = DISABLED_TEXT;
        else if (lit)
            c = VOID_BG;                       // 9.15:1 on the accent; white would be 2.09:1
        else if (role == Role::destructive)
            c = DESTRUCTIVE;
        else if (role == Role::quiet)
            c = highlighted ? TEXT_MAIN : TEXT_DIM;
        else
            c = TEXT_MAIN;

        g.setColour(c);
        g.setFont(getTextButtonFont(button, button.getHeight()));
        g.drawFittedText(button.getButtonText(),
                         button.getLocalBounds().reduced(button.getHeight() > 20 ? 8 : 4, 0),
                         Justification::centred, 2, 1.0f);

        ignoreUnused(down);
    }

    // The rest of these delegate and then add what Obsidian leaves out, so the family
    // look stays in one place and this file does not fork the drawing.

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool highlighted, bool down) override
    {
        okstudio::obsidian::LookAndFeel::drawToggleButton(g, button, highlighted, down);

        if (shouldShowFocus(button))
        {
            // Ring the box, not the whole component: the label runs off to the right and
            // a ring around all of it would read as a selected row.
            const float boxS = 20.0f;
            const auto bounds = button.getLocalBounds().toFloat();
            const auto box = juce::Rectangle<float>(boxS, boxS)
                                 .withCentre({ bounds.getX() + boxS * 0.5f + 1.0f, bounds.getCentreY() });
            focusRing(g, box, 5.0f, okstudio::obsidian::accentOf(button).base);
        }
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool isDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override
    {
        namespace ob = okstudio::obsidian;
        ob::LookAndFeel::drawComboBox(g, width, height, isDown, buttonX, buttonY, buttonW, buttonH, box);

        const auto r = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height).reduced(1.0f);

        // A combo box is a control and owes the same 3:1 boundary a button does.
        g.setColour(box.isEnabled() ? CONTROL_BORDER : DISABLED_BORDER);
        g.drawRoundedRectangle(r, ob::radius, 1.0f);

        if (shouldShowFocus(box))
            focusRing(g, r, ob::radius, ob::accentOf(box).base);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider& slider) override
    {
        namespace ob = okstudio::obsidian;
        ob::LookAndFeel::drawRotarySlider(g, x, y, width, height, sliderPos, startAngle, endAngle, slider);

        if (shouldShowFocus(slider))
        {
            // Round the ring to the knob, which is the square inscribed in the bounds.
            const auto side = (float) juce::jmin(width, height);
            const auto knob = juce::Rectangle<float>(side, side)
                                  .withCentre({ (float) x + width * 0.5f, (float) y + height * 0.5f });
            focusRing(g, knob.reduced(1.0f), side * 0.5f, ob::accentOf(slider).base);
        }
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        namespace ob = okstudio::obsidian;
        ob::LookAndFeel::drawLinearSlider(g, x, y, width, height, sliderPos,
                                          minSliderPos, maxSliderPos, style, slider);

        if (shouldShowFocus(slider))
        {
            const auto r = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height);
            focusRing(g, r.reduced(1.0f), ob::radius, ob::accentOf(slider).base);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LookAndFeel)
};
} // namespace quarry::lnf

#endif // QUARRY_LOOKANDFEEL_H
