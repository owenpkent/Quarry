#pragma once

#include <juce_graphics/juce_graphics.h>

#include "Obsidian.h"

/*
    The line's typeface, and the one call that installs it.

    The faces themselves are in data/fonts/ rather than in this header. Icons.h can hold its
    artwork because SVG is text and nineteen of them are twelve kilobytes; three TTFs are five
    hundred and eighty, and a header of byte arrays would be several megabytes of source that
    every consumer recompiles. So a product embeds the files the way it embeds its own resources
    -- juce_add_binary_data, or whatever its build already does -- and passes the bytes here.

    Why the kit ships a face at all. Before this, ui() asked for "Segoe UI" by name and every
    product in the line drew its chrome in whatever the machine happened to have: Segoe on
    Windows, something else on macOS, nothing chosen anywhere. Five of the six embedded no font
    at all. "The line looks like the line" is not something a per-machine fallback can deliver,
    and it is the whole reason a shared kit exists.

    Montserrat because Quarry already shipped it, already licensed it and built a wordmark on it,
    so adopting it line-wide costs the one product that has an identity nothing at all. If the
    line ever wants a face chosen for interface text rather than inherited from the first product
    to need one, that is a decision about three files in data/fonts and this header does not
    change.

    ------------------------------------------------------------------------------------------
    Montserrat is SIL Open Font License 1.1. The licence and the copyright notice the shipped
    faces carry are in data/fonts/OFL.txt, which travels with them; a consumer that redistributes
    the fonts -- which is what embedding them in a binary is -- has to carry that notice too.
    ------------------------------------------------------------------------------------------
*/

namespace okstudio::fonts
{

/** The files in data/fonts, named so a build script and a consumer cannot disagree about them. */
inline constexpr const char* regularFile = "Montserrat-Regular.ttf";
inline constexpr const char* semiBoldFile = "Montserrat-SemiBold.ttf";
inline constexpr const char* boldFile = "Montserrat-Bold.ttf";

/** The licence that must ship beside them. */
inline constexpr const char* licenceFile = "OFL.txt";

/**
 * Hand the line's faces to the chrome, from bytes a product has embedded.
 *
 * One call rather than each product writing out createSystemTypefaceFor twice and remembering
 * to pass the result to obsidian::setUiTypefaces, because "each product does the same three
 * lines correctly" is exactly the assumption that left the line with no typeface in the first
 * place.
 *
 * Call it on the message thread before the components are built, for the reason given at
 * obsidian::setUiTypefaces: some chrome keeps the font it was made with. It is safe to call
 * again. A plugin calls it from every editor it opens, and once the faces are in, it returns
 * true without loading them a second time.
 *
 * Returns false, and installs nothing, if either face cannot be made from its bytes: a wrong
 * BinaryData symbol or size would otherwise be a window quietly drawn in two typefaces.
 *
 * It does two things. Obsidian's chrome (menus, buttons, combo boxes, tooltips) draws in these
 * faces, through obsidian::setUiTypefaces. And the regular becomes the default sans of the
 * default LookAndFeel, because the rest of the kit asks for no face by name: RangeKnob's
 * readout, theme::LookAndFeel, and every JUCE default the kit leaves alone (labels, alert
 * windows, slider popups) draw in the default sans. A product that wants a different default
 * for its own text sets it after this call; one that wants the bold for a title loads it the
 * same way these are loaded.
 */
inline bool useEmbedded(const void* inRegularData,
                        int inRegularSize,
                        const void* inSemiBoldData,
                        int inSemiBoldSize)
{
    if (obsidian::hasUiTypefaces())
        return true;

    if (inRegularData == nullptr || inRegularSize <= 0 || inSemiBoldData == nullptr || inSemiBoldSize <= 0)
        return false;

    auto regular = juce::Typeface::createSystemTypefaceFor(inRegularData, (size_t) inRegularSize);
    auto semiBold = juce::Typeface::createSystemTypefaceFor(inSemiBoldData, (size_t) inSemiBoldSize);

    if (regular == nullptr || semiBold == nullptr)
        return false;

    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface(regular);
    obsidian::setUiTypefaces(regular, semiBold);
    return true;
}

} // namespace okstudio::fonts
