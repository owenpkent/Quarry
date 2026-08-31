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

/** The icons are Lucide, taken as published and so stroked in black (okstudio/Icons.h says why),
    and black on Obsidian's ground is 1.09:1. Every one of them is repainted on load against the
    colour the surface wants: TEXT_MAIN at 14.8:1, and still 3.5:1 once JUCE dims a disabled
    button to 0.41 alpha, which most of the toolbar is until a take exists.

    Here rather than beside one of its callers because there are now two -- the toolbar and the
    footer -- and an icon repainted one way in one place and another way in the other is a
    contrast bug nobody would think to look for. */
inline void recolourIcon(juce::Drawable* inDrawable, juce::Colour inColour)
{
    if (inDrawable == nullptr)
        return;

    // Two blacks, not one. Lucide's stroke is #000000; 0e0e0e is the near-black the icons this
    // set replaced were authored in for a light theme that no longer exists, and it stays until
    // nothing in Assets is drawn in it any more.
    inDrawable->replaceColour(juce::Colour(0xff0e0e0e), inColour);
    inDrawable->replaceColour(juce::Colours::black, inColour);
}

/** An icon from its SVG source, already repainted. The SVG is a string constant, so the two
    arguments JUCE wants are the one thing a caller has. */
inline std::unique_ptr<juce::Drawable> icon(const void* inSvgData, int inSvgSize, juce::Colour inColour)
{
    auto drawable = juce::Drawable::createFromImageData(inSvgData, (size_t) inSvgSize);
    recolourIcon(drawable.get(), inColour);
    return drawable;
}

inline Role roleOf(const juce::Component& c)
{
    const auto v = c.getProperties().getWithDefault(rolePropertyId(), static_cast<int>(Role::secondary));
    return static_cast<Role>(static_cast<int>(v));
}

// The focus ring: 2px of accent just inside the control, per docs/ACCESSIBILITY.md 3.
//
// Inside, and this is the whole of why the first version of this drew nothing at all.
// JUCE clips a component's painting to its own bounds unless it opts out with
// setPaintingIsUnclipped, which nothing here does (juce_Component.cpp, paintComponentAndChildren).
// Callers pass the control's paint rect, which is the local bounds less a pixel, so a ring
// expanded outwards from it landed one to three pixels beyond the component. Every straight
// edge of it was clipped away; only fragments of the rounded corners, which curve back
// inside, survived. Tests/focus_ring_test.h measures it at 72 pixels of 632. Every control
// in the product was focusable, every one drew a ring, and what reached the screen was four
// specks in the corners.
//
// Drawing inward costs half a pixel of overlap with the control's own border, which is
// cheap and reads correctly: a focused control shows its outline with an accent ring
// seated just inside it. The alternative, reserving two pixels of padding around every
// control so the ring has somewhere to go, would have shrunk every control in the
// product to fix a ring nobody had seen yet.
inline void focusRing(juce::Graphics& g, juce::Rectangle<float> r, float cornerRadius,
                      juce::Colour accent)
{
    g.setColour(accent);
    g.drawRoundedRectangle(r.reduced(1.0f), juce::jmax(0.0f, cornerRadius - 1.0f), 2.0f);
}

// Half the void left between one band and the next. Taken off the top of a band's first row
// and the bottom of its last, so a band of one row loses twice this and still centres.
constexpr int bandGap = 4;

// Where a row sits in its band. Both true is a band of one row, which is what a flat list
// passes and what a single-window application gets.
struct BandEdges
{
    bool first = true, last = true;
};

// The background of one list row: the alternating band, and the selection on top of it.
//
// inBandIndex is what alternates, and it is deliberately not the row number. The band has
// to follow the structure of the list or it fights it: in the sources list a band is one
// application, so all of Chrome's windows share a tint and the next application flips. An
// every-other-row stripe cut straight through those groups and split each application into
// striped and unstriped halves, which is worse than no band at all. A flat list with no
// groups passes its row number and gets the ordinary stripe.
//
// Selection cannot be carried by fill here, and the reason is worth keeping. TEXT_DIM is
// drawn on selected rows - the "source guessed" caption keeps it even when the row is
// chosen - which caps any row fill at about CONTROL_BG before TEXT_DIM drops under 4.5:1.
// At that cap the selected fill is 1.15:1 against the ground and 1.07:1 against the band,
// which is nothing. So the accent bar down the left edge is the signal, at 7.89:1, and the
// fill is only there to warm the row. That also satisfies SC 1.4.1: the selection is a
// shape as well as a colour, so it survives being colour blind or greyscale.
inline void listRowBackground(juce::Graphics& g, juce::Rectangle<int> inRow, int inBandIndex,
                              bool inSelected, juce::Colour inAccent, BandEdges inEdges = {})
{
    // Square, and the full row width. A band spans several rows, so rounded corners on each
    // would scallop its edges and undo the run the band exists to make.
    auto body = inRow.toFloat();

    // The gap between one application and the next, and the most important few pixels here.
    //
    // Nielsen Norman: proximity "can overpower competing visual cues such as similarity of
    // color or shape". Until this went in, every row sat exactly rowHeight from the next
    // whatever it belonged to, so the strongest grouping signal available was the one not
    // being used, and the band and the rule were left doing a job they are worse at.
    //
    // JUCE's ListBox rows are a fixed height, so the gap has to come out of the band rather
    // than out of the spacing between rows. Insetting the band's first and last row leaves
    // a real void between one common region and the next, which is the part the eye reads.
    if (inEdges.first)
        body.removeFromTop((float) bandGap);
    if (inEdges.last)
        body.removeFromBottom((float) bandGap);

    if (inBandIndex % 2 == 1)
    {
        g.setColour(ROW_ALT);
        g.fillRect(body);
    }

    if (! inSelected)
        return;

    g.setColour(CONTROL_BG);
    g.fillRect(body);

    g.setColour(inAccent);
    g.fillRect(body.withWidth(3.0f));
}

// The rule down a group's gutter, drawn full height so consecutive rows of one application
// join into one unbroken line rather than a column of dashes.
//
// CONTROL_BORDER because this carries grouping, which is information: HAIRLINE is 1.06:1
// against the row and would draw nothing. The band is the primary signal and this
// reinforces it, which is the pair SC 1.4.1 wants - never one channel alone.
inline void listGroupRule(juce::Graphics& g, juce::Rectangle<int> inRow, int inX, BandEdges inEdges = {})
{
    auto span = inRow;

    // Stop where the band stops. A rule that ran into the gap would bridge the very void
    // that separates one application from the next.
    if (inEdges.first)
        span.removeFromTop(bandGap);
    if (inEdges.last)
        span.removeFromBottom(bandGap);

    g.setColour(CONTROL_BORDER);
    g.fillRect(inX, span.getY(), 1, span.getHeight());
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
    LookAndFeel()
    {
        // Obsidian styles every part of a popup menu except its section headings, because
        // nothing in the line has grouped a menu before. Left alone, the heading takes
        // LookAndFeel_V4's scheme colour, which is not one of the four Obsidian sets, and lands
        // as near-white text the same size as the rows it is meant to sit above.
        setColour(juce::PopupMenu::headerTextColourId, TEXT_DIM);
    }

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

    // The one number this class copies from Obsidian, whose popup menu draws every row's text
    // into area.reduced(26, 0) and offers no constant to ask for it. A heading that does not sit
    // on the same left edge as the rows it labels is a heading over nothing, so this has to
    // agree with that inset; menuGutterAllowance below is where the agreement is checked.
    static constexpr int menuTextGutter = 26;

    // A section heading in a grouped menu, e.g. the MODEL picker's "SOLO PIANO".
    //
    // The base draws it in the menu's own font, boldened, outdented twelve pixels from a row
    // that Obsidian indents twenty-six. That is a heading only by weight, and it lines up with
    // nothing. This is the same caps micro-label the panels use, on the row's own left edge, so
    // the group reads as a label over a list rather than as a louder list item.
    void drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area,
                                    const juce::String& sectionName) override
    {
        g.setColour(findColour(juce::PopupMenu::headerTextColourId));
        g.setFont(UIDefines::LABEL_FONT());
        g.drawText(sectionName, area.reduced(menuTextGutter, 0).withTrimmedBottom(3),
                   juce::Justification::bottomLeft, true);
    }

    // Obsidian sizes every row to minHitPx, which is right for a row you have to hit and wrong
    // for a heading, which is not a target and cannot be clicked. Left at the row height, four
    // headings add a hundred and thirty-six pixels of nothing to a seven-item menu.
    void getIdealPopupMenuSectionHeaderSizeWithOptions(const juce::String& title, int,
                                                       int& idealWidth, int& idealHeight,
                                                       const juce::PopupMenu::Options&) override
    {
        const auto f = UIDefines::LABEL_FONT();
        idealWidth = (int) std::ceil(f.getStringWidthFloat(title)) + menuGutterAllowance();
        idealHeight = 26;
    }

    // What Obsidian adds to a string's width to make a row: its two gutters plus its own slack.
    // Asked for rather than copied -- an empty item sizes to the allowance and nothing else -- so
    // a heading cannot end up narrower than the rows beneath it if that arithmetic ever moves.
    // The two numbers it is built from are documented in Obsidian as having to agree with each
    // other; this keeps them agreeing with a third place across a vendored-header boundary.
    int menuGutterAllowance()
    {
        int allowance = 0, unusedHeight = 0;
        okstudio::obsidian::LookAndFeel::getIdealPopupMenuItemSize({}, false, 0, allowance, unusedHeight);

        // If Obsidian's gutter ever shrinks below what drawPopupMenuSectionHeader insets by, the
        // heading starts hanging off the left of its own rows.
        jassert(allowance >= menuTextGutter * 2);

        return allowance;
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
            // Round the ring to the knob, which is the square inscribed in the bounds. The
            // radius is that of the rect actually handed over, so the ring stays a circle
            // rather than a rounded square a pixel too generous at the corners.
            const auto side = (float) juce::jmin(width, height);
            const auto knob = juce::Rectangle<float>(side, side)
                                  .withCentre({ (float) x + width * 0.5f, (float) y + height * 0.5f });
            focusRing(g, knob.reduced(1.0f), side * 0.5f - 1.0f, ob::accentOf(slider).base);
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
