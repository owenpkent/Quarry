//
// Created by Damien Ronssin on 12.03.23.
//

#ifndef NN_UIDEFINES_H
#define NN_UIDEFINES_H

#include <JuceHeader.h>
#include "BinaryData.h"

namespace UIDefines
{
inline Typeface::Ptr MONTSERRAT_REGULAR()
{
    static const auto font =
        Typeface::createSystemTypefaceFor(BinaryData::MontserratRegular_ttf, BinaryData::MontserratRegular_ttfSize);
    return font;
}

inline Typeface::Ptr MONTSERRAT_SEMIBOLD()
{
    static const auto font =
        Typeface::createSystemTypefaceFor(BinaryData::MontserratSemiBold_ttf, BinaryData::MontserratSemiBold_ttfSize);
    return font;
}

inline Typeface::Ptr MONTSERRAT_BOLD()
{
    static const auto font =
        Typeface::createSystemTypefaceFor(BinaryData::MontserratBold_ttf, BinaryData::MontserratBold_ttfSize);
    return font;
}

inline Font TITLE_FONT()
{
    static const auto font = Font(FontOptions(MONTSERRAT_BOLD())).withPointHeight(18.0f);
    return font;
}

inline Font LARGE_FONT()
{
    static const auto font = Font(FontOptions(MONTSERRAT_BOLD())).withPointHeight(20.0f);
    return font;
}

inline Font LABEL_FONT()
{
    static const auto font = Font(FontOptions(MONTSERRAT_SEMIBOLD())).withPointHeight(10.0f);
    return font;
}

inline Font DROPDOWN_FONT()
{
    static const auto font = Font(FontOptions(MONTSERRAT_REGULAR())).withPointHeight(10.0f);
    return font;
}

inline Font BUTTON_FONT()
{
    static const auto font = Font(FontOptions(MONTSERRAT_BOLD())).withPointHeight(12.0f);
    return font;
}
} // namespace Fonts

// Colors
//
// These follow okstudio/Obsidian.h so the hand-painted components match the
// widgets the look and feel draws. Obsidian owns the values; anything named
// here is either a role Obsidian does not cover (piano keys, the record light)
// or a convenience alias for one it does. The accent is deliberately absent:
// it is user-selectable, so read it per component with
// okstudio::obsidian::accentOf(component) rather than freezing one here.

// Grounds, darkest to lightest.
static const Colour VOID_BG(static_cast<uint8>(0x0e), static_cast<uint8>(0x0f), static_cast<uint8>(0x12));
static const Colour WELL_BG(static_cast<uint8>(0x10), static_cast<uint8>(0x12), static_cast<uint8>(0x16));
static const Colour PANEL_BG(static_cast<uint8>(0x22), static_cast<uint8>(0x25), static_cast<uint8>(0x2b));
// Panels are drawn with okstudio::obsidian::raisedFill, which needs a top and a
// bottom for its gradient. A flat fill this close to the ground disappears.
static const Colour PANEL_TOP(static_cast<uint8>(0x26), static_cast<uint8>(0x2a), static_cast<uint8>(0x31));
static const Colour PANEL_BOT(static_cast<uint8>(0x1c), static_cast<uint8>(0x1f), static_cast<uint8>(0x24));
static const Colour CONTROL_BG(static_cast<uint8>(0x26), static_cast<uint8>(0x2a), static_cast<uint8>(0x31));
static const Colour HAIRLINE(static_cast<uint8>(0x2a), static_cast<uint8>(0x2e), static_cast<uint8>(0x35));

// The boundary of an interactive control, and the answer to WCAG SC 1.4.11.
//
// CONTROL_BG and PANEL_TOP are the same value, so a button drawn on the top of a panel
// has no fill separation from it whatsoever. The whole surface ramp spans #0e0f12 to
// #262a31, which is not enough range to carry a 3:1 boundary by fill: reaching it would
// mean a light grey button, which is not what this product looks like. So a control is
// identified by its border and its fill stays dark.
//
// This is the dimmest cool grey clearing 3:1 against every surface a control can sit on
// (3.04 on CONTROL_BG, 3.24 on PANEL_BG, 3.96 on WELL_BG, 4.05 on VOID_BG). Do not reach
// for HAIRLINE here: it is 1.06:1 against CONTROL_BG and delimits nothing. Do not draw
// this with alpha, which would put it back under 3:1.
static const Colour CONTROL_BORDER(static_cast<uint8>(0x6e), static_cast<uint8>(0x73), static_cast<uint8>(0x81));

// Every other row in a list, over the PANEL_BOT the list is drawn on. Its only job is to
// let the eye carry across a wide row without losing the line, so it is deliberately faint:
// 1.06:1 against the ground.
//
// It cannot be any lighter. TEXT_DIM is drawn on these rows and clears 4.5:1 against this
// by 0.39; the next step up the ramp puts it under. A stripe strong enough to read as a
// state would also be indistinguishable from a selected row, which is the thing that has
// to stand out.
static const Colour ROW_ALT(static_cast<uint8>(0x22), static_cast<uint8>(0x24), static_cast<uint8>(0x29));

// Text, brightest to dimmest.
static const Colour TEXT_MAIN(static_cast<uint8>(0xe9), static_cast<uint8>(0xec), static_cast<uint8>(0xf0));
static const Colour TEXT_DIM(static_cast<uint8>(0x8a), static_cast<uint8>(0x91), static_cast<uint8>(0x9c));
// There is no third text tier. A fainter one was tried and every use of it was
// text, so it answered to WCAG 2.2 SC 1.4.3 and its 4.5:1, not to the 3:1
// SC 1.4.11 asks of graphical objects. These labels are not drawn on the window
// ground, they are drawn on the raised panels, whose gradient runs PANEL_TOP to
// PANEL_BOT, and the faintest step clearing 4.5:1 against PANEL_TOP is #8a919b,
// which is TEXT_DIM. So the quiet tier and the dim tier are the same colour, and
// keeping two names for it only invited the next person to pick the failing one.
// A genuinely fainter label needs a darker plate, not a darker grey.

// Piano roll keys. Not an Obsidian role: the keyboard has to read as a keyboard.
static const Colour KEY_WHITE(static_cast<uint8>(0xc9), static_cast<uint8>(0xce), static_cast<uint8>(0xd6));
static const Colour KEY_BLACK(static_cast<uint8>(0x15), static_cast<uint8>(0x18), static_cast<uint8>(0x1c));

static const Colour WAVEFORM_BG_COLOR(static_cast<uint8>(0x0e), static_cast<uint8>(0x0f), static_cast<uint8>(0x12));
static const Colour RECORD_RED(216, 74, 96);
static const Colour TRANSPARENT(static_cast<uint8>(0), static_cast<uint8>(0), static_cast<uint8>(0), 0.0f);

// Halving the alpha of light text on a dark ground makes it unreadable, where
// halving dark text on a light one merely greys it. This is the dark-theme
// equivalent of the 0.5 the light theme used.
static constexpr float DISABLED_ALPHA = 0.78f;

// Distances

static constexpr int LEFT_SECTIONS_TOP_PAD = 24;

#endif //NN_UIDEFINES_H
