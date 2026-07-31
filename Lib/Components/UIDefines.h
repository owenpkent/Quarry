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

// Text, brightest to dimmest.
static const Colour TEXT_MAIN(static_cast<uint8>(0xe9), static_cast<uint8>(0xec), static_cast<uint8>(0xf0));
static const Colour TEXT_DIM(static_cast<uint8>(0x8a), static_cast<uint8>(0x91), static_cast<uint8>(0x9c));
static const Colour TEXT_FAINT(static_cast<uint8>(0x5a), static_cast<uint8>(0x60), static_cast<uint8>(0x68));

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
