#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MouseOnly.h"

// Obsidian: a dimensional dark skin for the OK Studio line, in the spirit of the
// great soft-synth panels (near-black neutral chrome, one glowing accent, machined
// knobs, micro-caps labels). Vector-drawn only (gradients + layered strokes, no
// images, no OpenGL), so it scales with a resizable editor and stays cheap on the
// message thread.
//
// Promoted from Keys' src/ui/KeysLookAndFeel.h, where it first shipped as a
// product-specific restyle of okstudio::theme::LookAndFeel. Keys keeps its own
// KeysLookAndFeel.h untouched and does not consume this header, so the two copies
// will drift apart unless Keys is later migrated to sit on top of this one. This
// copy is purely additive: nothing elsewhere in the kit references it, so adopting
// it is exactly
//
//     someEditor.setLookAndFeel(&myObsidianLookAndFeel);
//
// The accent is per instance, not a global. A DAW loads every plugin instance into
// one process, so a global accent would repaint every track's instance at once
// whenever any one of them changed colour. Each consumer owns one
// okstudio::obsidian::LookAndFeel (typically one per editor), picks a colour with
// setAccent(), and components read it back through accentOf(), which walks the
// LookAndFeel chain JUCE already maintains up to the editor - a component never
// needs to know who owns it.
namespace okstudio::obsidian
{
    // Chrome: a neutral charcoal, slightly cool, never blue.
    const juce::Colour bgTop      { 0xff17181c };
    const juce::Colour bgBot      { 0xff0e0f12 };
    const juce::Colour headerTop  { 0xff1c1f24 };
    const juce::Colour headerBot  { 0xff16181d };
    const juce::Colour panel      { 0xff1a1c21 };  // raised module strip
    const juce::Colour well       { 0xff101216 };  // inset grooves + value wells
    const juce::Colour control    { 0xff262a31 };  // raised control top
    const juce::Colour controlBot { 0xff1f2227 };

    // The accent: a base, a hot core and a deep shade for gradient ends. Every lit
    // state on every surface (knob arcs, toggle fills, popup highlights) uses this
    // family. Cyan is the default and the line's colour; the rest exist to tell
    // instances apart when a session has more than one of the same plugin open.
    struct Accent
    {
        juce::Colour base, hot, deep;
    };

    // The default: the OK Studio cyan, with its shipped hot/deep pair kept exact
    // rather than derived, since the whole skin was tuned against these three.
    const Accent cyanAccent { juce::Colour(0xff35c4d7),
                              juce::Colour(0xff8fe8f2),
                              juce::Colour(0xff1b8496) };

    // Everything else is derived from one base, so adding a colour is one line.
    inline Accent derive(juce::Colour base)
    {
        return { base, base.brighter(0.75f), base.darker(0.45f) };
    }

    struct AccentChoice
    {
        const char* name;
        juce::uint32 argb; // 0 = use cyanAccent, which is not derived
    };
    inline const AccentChoice* accentChoices()
    {
        static const AccentChoice table[] = {
            { "Cyan",    0 },
            { "Amber",   0xffd7a635 },
            { "Lime",    0xff8fd735 },
            { "Violet",  0xff9a6cf5 },
            { "Magenta", 0xffd7459f },
            { "Orange",  0xffe0703a },
            { "Rose",    0xffe04a6b },
            { "Ice",     0xff8fb4de },
        };
        return table;
    }
    constexpr int numAccents = 8;

    inline Accent accentAt(int index)
    {
        index = juce::jlimit(0, numAccents - 1, index);
        const auto& choice = accentChoices()[index];
        return choice.argb == 0 ? cyanAccent : derive(juce::Colour(choice.argb));
    }

    // The accent of whichever consumer's LookAndFeel this component sits under.
    // Declared here, next to the rest of the accent API; the definition sits just
    // after the LookAndFeel class below, the first point in the file where that type
    // is complete enough to dynamic_cast to. Falls back to cyan for anything
    // painting outside an Obsidian-skinned editor (e.g. a bare unit test).
    Accent accentOf(const juce::Component&);

    const juce::Colour text      { 0xffe9ecf0 };
    const juce::Colour textDim   { 0xff8a919c };
    const juce::Colour textFaint { 0xff5a6068 };

    constexpr float radius = 6.0f;       // controls
    constexpr float panelRadius = 8.0f;  // panels / modules

    // Segoe UI keeps the panel crisp on Windows (the shipping target) and falls
    // back to the platform sans elsewhere; nothing is embedded.
    inline juce::Font ui(float height)
    {
        return juce::Font(juce::FontOptions("Segoe UI", height, juce::Font::plain));
    }
    inline juce::Font uiSemi(float height)
    {
        return juce::Font(juce::FontOptions("Segoe UI Semibold", height, juce::Font::plain));
    }
    // Micro-caps section labels; callers pass uppercase text.
    inline juce::Font micro(float height = 10.0f)
    {
        return uiSemi(height).withExtraKerningFactor(0.08f);
    }

    // A raised chip: soft vertical gradient, dark seat line, 1 px top catch-light.
    inline void raisedFill(juce::Graphics& g, juce::Rectangle<float> r, float cornerRadius,
                           juce::Colour top, juce::Colour bottom, bool topHighlight = true)
    {
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.drawRoundedRectangle(r.expanded(0.5f), cornerRadius + 0.5f, 1.0f);
        g.setGradientFill({ top, 0.0f, r.getY(), bottom, 0.0f, r.getBottom(), false });
        g.fillRoundedRectangle(r, cornerRadius);
        if (topHighlight)
        {
            g.setColour(juce::Colours::white.withAlpha(0.055f));
            g.fillRoundedRectangle(r.withHeight(1.5f).reduced(cornerRadius * 0.5f, 0.0f), 0.75f);
        }
    }

    // Two-pass accent halo around a rounded rect (tight bright pass + wide soft pass).
    inline void glowRect(juce::Graphics& g, juce::Rectangle<float> r, float cornerRadius,
                         juce::Colour colour, float strength = 1.0f)
    {
        g.setColour(colour.withAlpha(0.14f * strength));
        g.drawRoundedRectangle(r.expanded(2.0f), cornerRadius + 2.0f, 3.5f);
        g.setColour(colour.withAlpha(0.45f * strength));
        g.drawRoundedRectangle(r.expanded(0.5f), cornerRadius + 0.5f, 1.2f);
    }

    // A lit metal ball thumb: drop shadow, top-lit sphere, specular dot.
    inline void ballThumb(juce::Graphics& g, juce::Point<float> centre, float radiusPx)
    {
        g.setColour(juce::Colours::black.withAlpha(0.30f));
        g.fillEllipse(juce::Rectangle<float>(radiusPx * 2.1f, radiusPx * 2.1f)
                          .withCentre(centre.translated(0.0f, 1.5f)));

        juce::ColourGradient body(juce::Colour(0xff474c55), centre.x - radiusPx * 0.35f, centre.y - radiusPx * 0.45f,
                                  juce::Colour(0xff1e2126), centre.x + radiusPx * 0.6f, centre.y + radiusPx, true);
        g.setGradientFill(body);
        g.fillEllipse(juce::Rectangle<float>(radiusPx * 2.0f, radiusPx * 2.0f).withCentre(centre));

        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawEllipse(juce::Rectangle<float>(radiusPx * 2.0f, radiusPx * 2.0f).withCentre(centre), 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.22f));
        g.fillEllipse(juce::Rectangle<float>(radiusPx * 0.9f, radiusPx * 0.55f)
                          .withCentre({ centre.x - radiusPx * 0.25f, centre.y - radiusPx * 0.45f }));
    }

    // Tooltip layout helpers, private to drawTooltip()/getTooltipBounds() below.
    // JUCE's default tooltip is a 13 px font in a box up to 400 px wide, which next
    // to this skin's 10 px micro-caps reads like a different application shouting.
    namespace detail
    {
        inline juce::Font tooltipFont() { return ui(11.5f); }
        constexpr int tooltipMaxWidth = 260; // wrap sooner than JUCE's 400
        constexpr int tooltipPadX = 9, tooltipPadY = 5;

        inline juce::TextLayout layoutTooltip(const juce::String& text_, int maxWidth)
        {
            juce::AttributedString s;
            s.setJustification(juce::Justification::centredLeft);
            s.append(text_, tooltipFont(), text);
            juce::TextLayout layout;
            layout.createLayout(s, (float) maxWidth);
            return layout;
        }
    } // namespace detail

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            using namespace juce;
            setColour(ResizableWindow::backgroundColourId, bgBot);
            setColour(Label::textColourId, text);
            setColour(Label::outlineColourId, Colours::transparentBlack); // V4 default draws a light box

            setColour(ComboBox::backgroundColourId, control);
            setColour(ComboBox::textColourId, text);
            setColour(ComboBox::outlineColourId, Colours::transparentBlack);
            setColour(ComboBox::arrowColourId, textDim);

            setColour(PopupMenu::backgroundColourId, Colour(0xff1e2127));
            setColour(PopupMenu::textColourId, text);
            setColour(PopupMenu::highlightedTextColourId, Colour(0xffeafcff));

            setColour(TextButton::buttonColourId, control);
            setColour(TextButton::textColourOffId, text);
            setColour(TextButton::textColourOnId, Colour(0xffeafcff));

            setColour(Slider::backgroundColourId, well);
            setColour(Slider::textBoxTextColourId, text);
            setColour(Slider::textBoxBackgroundColourId, Colours::transparentBlack); // values float
            setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);

            setColour(TextEditor::backgroundColourId, well);
            setColour(TextEditor::textColourId, text);
            setColour(TextEditor::outlineColourId, Colours::transparentBlack);

            setColour(ToggleButton::textColourId, text);
            setColour(ToggleButton::tickDisabledColourId, textDim);

            setAccent(0); // fills in every ColourId derived from the accent
        }

        // One per consumer (typically one per editor), so each instance wears its
        // own colour. Re-applies the JUCE ColourIds the skin derives from the accent
        // (tick marks, slider tracks, the popup highlight), which are baked at
        // construction and would otherwise stay cyan.
        void setAccent(int newIndex)
        {
            using namespace juce;
            index = jlimit(0, numAccents - 1, newIndex);
            accentColours = accentAt(index);
            const auto a = accentColours;

            setColour(PopupMenu::highlightedBackgroundColourId, a.base.withAlpha(0.15f));
            setColour(TextButton::buttonOnColourId, control.interpolatedWith(a.base, 0.32f));
            setColour(Slider::trackColourId, a.base);
            setColour(Slider::thumbColourId, a.hot);
            setColour(TextEditor::highlightColourId, a.base.withAlpha(0.35f));
            setColour(TextEditor::focusedOutlineColourId, a.base.withAlpha(0.6f));
            setColour(ToggleButton::tickColourId, a.base);
        }
        Accent accent() const { return accentColours; }
        int accentIndex() const { return index; }

        // A machined knob: shadowed cap with a top-lit face and specular, a dark
        // groove arc, and a glowing accent value arc with a hot core.
        void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                              float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override
        {
            const bool enabled = slider.isEnabled();
            if (! enabled)
                g.beginTransparencyLayer(0.45f);

            const auto bounds = juce::Rectangle<float>((float) x, (float) y, (float) width, (float) height);
            const auto centre = bounds.getCentre();
            const float radiusPx = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 3.0f;
            const float lw = juce::jmax(2.5f, radiusPx * 0.15f);
            const float arcR = radiusPx - lw * 0.5f;
            const float capR = arcR - lw * 1.6f;
            const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

            // A knob whose range straddles zero fills *from the centre*, not from the
            // minimum: a bipolar control at 0 should look empty, and one turned left
            // should read as clearly negative rather than as half full.
            const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
            const float originPos = bipolar ? (float) slider.valueToProportionOfLength(0.0) : 0.0f;
            const float originAngle = rotaryStartAngle + originPos * (rotaryEndAngle - rotaryStartAngle);

            // Groove.
            juce::Path track;
            track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
            g.setColour(well);
            g.strokePath(track, { lw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

            // Centre detent mark, so a bipolar knob shows where "off" is at a glance.
            if (bipolar)
            {
                juce::Path tick;
                tick.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f,
                                   originAngle - 0.012f, originAngle + 0.012f, true);
                g.setColour(textFaint);
                g.strokePath(tick, { lw, juce::PathStrokeType::curved, juce::PathStrokeType::butt });
            }

            // Value arc: halo, body, hot core.
            if (std::abs(sliderPos - originPos) > 0.001f)
            {
                juce::Path value;
                value.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f,
                                    juce::jmin(originAngle, angle), juce::jmax(originAngle, angle), true);
                g.setColour(accent().base.withAlpha(0.16f));
                g.strokePath(value, { lw * 2.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
                g.setColour(accent().base.withAlpha(0.55f));
                g.strokePath(value, { lw * 1.15f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
                g.setColour(accent().hot);
                g.strokePath(value, { lw * 0.55f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
            }

            // Cap shadow, rim, face.
            {
                juce::ColourGradient shadow(juce::Colours::black.withAlpha(0.4f),
                                            centre.x, centre.y + capR * 0.18f,
                                            juce::Colours::transparentBlack,
                                            centre.x, centre.y + capR * 1.5f, true);
                g.setGradientFill(shadow);
                g.fillEllipse(juce::Rectangle<float>(capR * 2.9f, capR * 2.9f)
                                  .withCentre(centre.translated(0.0f, capR * 0.16f)));

                const auto rim = juce::Rectangle<float>(capR * 2.0f, capR * 2.0f).withCentre(centre);
                g.setGradientFill({ juce::Colour(0xff363b42), 0.0f, rim.getY(),
                                    juce::Colour(0xff15171a), 0.0f, rim.getBottom(), false });
                g.fillEllipse(rim);

                const auto face = rim.reduced(1.6f);
                g.setGradientFill({ juce::Colour(0xff3c4149), 0.0f, face.getY(),
                                    juce::Colour(0xff1d2025), 0.0f, face.getBottom(), false });
                g.fillEllipse(face);

                // Specular pool near the top of the face.
                juce::Graphics::ScopedSaveState clip(g);
                juce::Path faceClip;
                faceClip.addEllipse(face);
                g.reduceClipRegion(faceClip);
                g.setGradientFill({ juce::Colours::white.withAlpha(0.11f), 0.0f, face.getY(),
                                    juce::Colours::transparentWhite, 0.0f, face.getCentreY(), false });
                g.fillEllipse(face.withHeight(face.getHeight() * 0.55f).expanded(1.0f, 0.0f));
            }
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.drawEllipse(juce::Rectangle<float>(capR * 2.0f, capR * 2.0f).withCentre(centre), 1.1f);

            // Pointer on the cap, glow under a hot core.
            {
                const float t = juce::jmax(2.2f, capR * 0.16f);
                juce::Path pointer;
                pointer.addRoundedRectangle(-t * 0.5f, -capR * 0.86f, t, capR * 0.58f, t * 0.5f);
                pointer.applyTransform(juce::AffineTransform::rotation(angle).translated(centre));
                g.setColour(accent().base.withAlpha(0.30f));
                g.strokePath(pointer, juce::PathStrokeType(t * 1.6f, juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
                g.setColour(accent().hot);
                g.fillPath(pointer);
            }

            if (! enabled)
                g.endTransparencyLayer();
        }

        // Horizontal sliders: an inset groove, an accent fill with a soft halo, and
        // lit ball thumbs. Other styles fall back to the JUCE default.
        void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                              float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                              juce::Slider& slider) override
        {
            if (style != juce::Slider::LinearHorizontal && style != juce::Slider::TwoValueHorizontal)
            {
                juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                                       minSliderPos, maxSliderPos, style, slider);
                return;
            }

            const bool enabled = slider.isEnabled();
            if (! enabled)
                g.beginTransparencyLayer(0.4f);

            const float cy = (float) y + (float) height * 0.5f;
            const float thumbR = 7.5f;
            const auto trackR = juce::Rectangle<float>((float) x + thumbR, cy - 3.0f,
                                                       (float) width - thumbR * 2.0f, 6.0f);

            // Groove with an inner top shadow.
            g.setColour(well);
            g.fillRoundedRectangle(trackR, 3.0f);
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRoundedRectangle(trackR.withHeight(1.5f).reduced(2.0f, 0.0f), 0.75f);
            g.setColour(juce::Colours::white.withAlpha(0.04f));
            g.fillRoundedRectangle(trackR.withY(trackR.getBottom() - 1.0f).withHeight(1.0f).reduced(2.0f, 0.0f), 0.5f);

            const bool twoValue = style == juce::Slider::TwoValueHorizontal;
            const float fillL = twoValue ? minSliderPos : trackR.getX();
            const float fillR = twoValue ? maxSliderPos : sliderPos;
            if (fillR > fillL + 0.5f)
            {
                const auto fill = juce::Rectangle<float>(fillL, trackR.getY(), fillR - fillL, trackR.getHeight());
                g.setGradientFill({ accent().deep, fill.getX(), 0.0f, accent().base, fill.getRight(), 0.0f, false });
                g.fillRoundedRectangle(fill, 3.0f);
                g.setColour(accent().base.withAlpha(0.18f));
                g.drawRoundedRectangle(fill.expanded(1.5f), 4.0f, 2.5f);
            }

            if (twoValue)
            {
                ballThumb(g, { minSliderPos, cy }, thumbR);
                ballThumb(g, { maxSliderPos, cy }, thumbR);
            }
            else
            {
                ballThumb(g, { sliderPos, cy }, thumbR);
            }

            if (! enabled)
                g.endTransparencyLayer();
        }

        juce::Label* createSliderTextBox(juce::Slider& slider) override
        {
            auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
            label->setFont(ui(13.5f));
            // Values float on the panel; the base class may have baked stale colours
            // from whichever LookAndFeel was current when the slider was configured.
            label->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour(juce::Label::textColourId, text);
            label->setColour(juce::TextEditor::backgroundColourId, well);
            label->setColour(juce::TextEditor::outlineColourId, accent().base.withAlpha(0.4f));
            label->setColour(juce::TextEditor::textColourId, text);
            label->setColour(juce::TextEditor::highlightColourId, accent().base.withAlpha(0.35f));
            return label;
        }

        void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                  bool highlighted, bool down) override
        {
            const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
            const float dim = button.isEnabled() ? 1.0f : 0.45f;
            if (dim < 1.0f)
                g.beginTransparencyLayer(dim);

            // backgroundColour arrives resolved (buttonColourId / buttonOnColourId,
            // plus any per-button override), so shade from it.
            auto base = backgroundColour;
            if (down)
                base = base.darker(0.25f);
            else if (highlighted)
                base = base.brighter(0.12f);

            raisedFill(g, r, radius, base.brighter(down ? 0.0f : 0.05f),
                      base.darker(down ? 0.05f : 0.16f), ! down);

            if (button.getToggleState())
                glowRect(g, r, radius, accent().base);

            if (dim < 1.0f)
                g.endTransparencyLayer();
        }

        juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override
        {
            // Tall buttons carry their name larger; ordinary chrome buttons stay at 14.
            const float cap = buttonHeight >= 60 ? 17.0f : 14.0f;
            return uiSemi(juce::jmin(cap, (float) buttonHeight * 0.45f));
        }

        // Checkbox-style toggles: an inset well that fills with glowing accent and a
        // white check when on.
        void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool) override
        {
            const float dim = button.isEnabled() ? 1.0f : 0.45f;
            if (dim < 1.0f)
                g.beginTransparencyLayer(dim);

            const auto bounds = button.getLocalBounds().toFloat();
            const float boxS = 20.0f;
            const auto box = juce::Rectangle<float>(boxS, boxS)
                                 .withCentre({ bounds.getX() + boxS * 0.5f + 1.0f, bounds.getCentreY() });
            const bool on = button.getToggleState();

            if (on)
            {
                g.setGradientFill({ accent().hot, 0.0f, box.getY(), accent().base, 0.0f, box.getBottom(), false });
                g.fillRoundedRectangle(box, 5.0f);
                glowRect(g, box, 5.0f, accent().base, 0.9f);

                juce::Path check;
                check.startNewSubPath(box.getX() + boxS * 0.26f, box.getY() + boxS * 0.54f);
                check.lineTo(box.getX() + boxS * 0.44f, box.getY() + boxS * 0.72f);
                check.lineTo(box.getX() + boxS * 0.76f, box.getY() + boxS * 0.30f);
                g.setColour(juce::Colour(0xff07272c));
                g.strokePath(check, { 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
            }
            else
            {
                g.setColour(well);
                g.fillRoundedRectangle(box, 5.0f);
                g.setColour(juce::Colours::black.withAlpha(0.4f));
                g.fillRoundedRectangle(box.withHeight(1.5f).reduced(3.0f, 0.0f), 0.75f);
                g.setColour(juce::Colours::white.withAlpha(highlighted ? 0.16f : 0.07f));
                g.drawRoundedRectangle(box, 5.0f, 1.0f);
            }

            g.setColour(text.withAlpha(on ? 1.0f : 0.88f));
            g.setFont(uiSemi(13.0f));
            g.drawText(button.getButtonText(),
                       button.getLocalBounds().withTrimmedLeft((int) boxS + 9),
                       juce::Justification::centredLeft, true);

            if (dim < 1.0f)
                g.endTransparencyLayer();
        }

        void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                          juce::ComboBox& box) override
        {
            const float dim = box.isEnabled() ? 1.0f : 0.45f;
            if (dim < 1.0f)
                g.beginTransparencyLayer(dim);

            const auto r = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height).reduced(0.5f);
            const auto base = box.findColour(juce::ComboBox::backgroundColourId);
            raisedFill(g, r, radius, base.brighter(0.05f), base.darker(0.16f));

            if (box.isPopupActive())
                glowRect(g, r, radius, accent().base, 0.7f);

            // Chevron.
            const float cx = (float) width - 13.0f, cy = (float) height * 0.5f;
            juce::Path chevron;
            chevron.startNewSubPath(cx - 4.5f, cy - 2.5f);
            chevron.lineTo(cx, cy + 2.5f);
            chevron.lineTo(cx + 4.5f, cy - 2.5f);
            g.setColour(box.isPopupActive() ? accent().base : textDim);
            g.strokePath(chevron, { 1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

            if (dim < 1.0f)
                g.endTransparencyLayer();
        }

        juce::Font getComboBoxFont(juce::ComboBox&) override { return ui(14.0f); }

        void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds(10, 1, box.getWidth() - 32, box.getHeight() - 2);
            label.setFont(getComboBoxFont(box));
        }

        // Standalone window chrome (title bar + window buttons). Only the standalone
        // wrapper ever shows these; in a DAW the host owns the window.
        void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g, int w, int h,
                                        int titleSpaceX, int titleSpaceW, const juce::Image*,
                                        bool drawTitleTextOnLeft) override
        {
            const auto r = juce::Rectangle<float>(0.0f, 0.0f, (float) w, (float) h);
            g.setGradientFill({ juce::Colour(0xff15171b), 0.0f, 0.0f,
                                juce::Colour(0xff101216), 0.0f, r.getBottom(), false });
            g.fillRect(r);
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            g.fillRect(0.0f, r.getBottom() - 1.0f, r.getWidth(), 1.0f);

            // Window name in the wordmark's voice: tracked caps, dimmed when unfocused.
            g.setColour(text.withAlpha(window.isActiveWindow() ? 0.92f : 0.5f));
            g.setFont(uiSemi(14.0f).withExtraKerningFactor(0.10f));
            g.drawText(window.getName().toUpperCase(),
                       juce::Rectangle<int>(titleSpaceX, 0, titleSpaceW, h),
                       drawTitleTextOnLeft ? juce::Justification::centredLeft
                                           : juce::Justification::centred,
                       true);
        }

        juce::Button* createDocumentWindowButton(int buttonType) override
        {
            // Thin-line glyphs on an invisible pad; the pad (button bounds) is the
            // full title-bar-height square, so the mouse target stays comfortably large.
            const auto stroked = [](const juce::Path& p)
            {
                juce::Path out;
                juce::PathStrokeType(1.9f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded)
                    .createStrokedPath(out, p);
                return out;
            };

            juce::ShapeButton* button = nullptr;
            if (buttonType == juce::DocumentWindow::closeButton)
            {
                juce::Path x;
                x.startNewSubPath(0.0f, 0.0f);
                x.lineTo(10.0f, 10.0f);
                x.startNewSubPath(10.0f, 0.0f);
                x.lineTo(0.0f, 10.0f);
                button = new juce::ShapeButton("close", textDim,
                                               juce::Colour(0xffff8a80), juce::Colour(0xffe25d5d));
                button->setShape(stroked(x), false, true, false);
            }
            else if (buttonType == juce::DocumentWindow::minimiseButton)
            {
                juce::Path dash;
                dash.startNewSubPath(0.0f, 5.0f);
                dash.lineTo(10.0f, 5.0f);
                button = new juce::ShapeButton("minimise", textDim, accent().hot, accent().base);
                button->setShape(stroked(dash), false, true, false);
            }
            else
            {
                juce::Path square;
                square.addRoundedRectangle(0.0f, 0.0f, 10.0f, 10.0f, 2.0f);
                button = new juce::ShapeButton("maximise", textDim, accent().hot, accent().base);
                button->setShape(stroked(square), false, true, false);
            }
            button->setBorderSize(juce::BorderSize<int>(13));
            return button;
        }

        // Tooltips: smaller type, tighter padding, narrower wrap than JUCE's default,
        // to match this skin's 10 px micro-caps rather than shouting over it.
        juce::Rectangle<int> getTooltipBounds(const juce::String& tip, juce::Point<int> screenPos,
                                              juce::Rectangle<int> parentArea) override
        {
            const auto layout = detail::layoutTooltip(tip, detail::tooltipMaxWidth);
            const int w = (int) std::ceil(layout.getWidth()) + detail::tooltipPadX * 2;
            const int h = (int) std::ceil(layout.getHeight()) + detail::tooltipPadY * 2;

            // Offset below-right of the pointer, flipped near an edge, then clamped
            // inside the parent - the same placement rule as JUCE's, just at our size.
            return juce::Rectangle<int>(
                       screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 18,
                       screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6,
                       w, h)
                .constrainedWithin(parentArea);
        }

        void drawTooltip(juce::Graphics& g, const juce::String& tipText, int width, int height) override
        {
            const auto r = juce::Rectangle<float>((float) width, (float) height);
            g.setColour(juce::Colour(0xf21e2127));
            g.fillRoundedRectangle(r, 4.0f);
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);

            detail::layoutTooltip(tipText, width - detail::tooltipPadX * 2)
                .draw(g, juce::Rectangle<float>((float) detail::tooltipPadX, (float) detail::tooltipPadY,
                                                (float) (width - detail::tooltipPadX * 2),
                                                (float) (height - detail::tooltipPadY * 2)));
        }

        void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override
        {
            const auto r = juce::Rectangle<float>(0.0f, 0.0f, (float) width, (float) height);
            g.fillAll(findColour(juce::PopupMenu::backgroundColourId));
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.drawRect(r, 1.0f);
        }

        // Must agree with drawPopupMenuItem's gutters, or JUCE sizes the menu to the
        // text alone and our own inset then clips it.
        void getIdealPopupMenuItemSize(const juce::String& itemText, bool isSeparator, int standardHeight,
                                       int& idealWidth, int& idealHeight) override
        {
            if (isSeparator)
            {
                idealWidth = 50;
                idealHeight = standardHeight > 0 ? standardHeight / 2 : 9;
                return;
            }

            // drawPopupMenuItem draws into area.reduced(26, 0): a left gutter for the
            // tick and a matching right one, which the base class's own sizing
            // doesn't know about.
            const auto f = getPopupMenuFont();
            idealWidth = (int) std::ceil(f.getStringWidthFloat(itemText)) + 26 * 2 + 10;
            idealHeight = juce::jmax(okstudio::ui::minHitPx, (int) std::ceil(f.getHeight() * 1.6f));
        }

        void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                               bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                               const juce::String& itemText, const juce::String& shortcutKeyText,
                               const juce::Drawable*, const juce::Colour* textColourToUse) override
        {
            if (isSeparator)
            {
                g.setColour(juce::Colours::white.withAlpha(0.07f));
                g.fillRect(area.reduced(8, 0).withHeight(1).withY(area.getCentreY()));
                return;
            }

            const auto r = area.toFloat().reduced(3.0f, 1.0f);
            if (isHighlighted && isActive)
            {
                g.setColour(accent().base.withAlpha(0.15f));
                g.fillRoundedRectangle(r, 4.0f);
                g.setColour(accent().base.withAlpha(0.4f));
                g.drawRoundedRectangle(r, 4.0f, 1.0f);
            }

            if (isTicked)
            {
                g.setColour(accent().base);
                g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f)
                                  .withCentre({ r.getX() + 10.0f, r.getCentreY() }));
            }

            auto colour = textColourToUse != nullptr ? *textColourToUse
                                                     : (isHighlighted ? juce::Colour(0xffeafcff) : text);
            g.setColour(isActive ? colour : textFaint);
            g.setFont(getPopupMenuFont());
            g.drawText(itemText, area.reduced(26, 0), juce::Justification::centredLeft, true);

            if (hasSubMenu)
            {
                const float cx = (float) area.getRight() - 12.0f, cy = (float) area.getCentreY();
                juce::Path arrow;
                arrow.startNewSubPath(cx - 2.0f, cy - 4.0f);
                arrow.lineTo(cx + 2.5f, cy);
                arrow.lineTo(cx - 2.0f, cy + 4.0f);
                g.setColour(textDim);
                g.strokePath(arrow, { 1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });
            }

            if (shortcutKeyText.isNotEmpty())
            {
                g.setColour(textDim);
                g.setFont(ui(12.0f));
                g.drawText(shortcutKeyText, area.reduced(26, 0), juce::Justification::centredRight, true);
            }
        }

        juce::Font getPopupMenuFont() override { return ui(13.5f); }

    private:
        Accent accentColours = cyanAccent;
        int index = 0;
    };

    inline Accent accentOf(const juce::Component& c)
    {
        // JUCE already walks the LookAndFeel up the hierarchy to the editor, so a
        // component gets its own instance's accent without knowing who owns it.
        if (auto* lnf = dynamic_cast<const LookAndFeel*>(&c.getLookAndFeel()))
            return lnf->accent();
        return cyanAccent;
    }
} // namespace okstudio::obsidian
