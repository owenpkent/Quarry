//
// Every icon in the window, actually parsed.
//
// The icons are SVG source from okstudio/Icons.h, turned into a Drawable at runtime by
// juce::Drawable::createFromImageData. That call returns a null pointer when the parse fails, and
// DrawableButton::setImages takes a null pointer without complaint, so an icon JUCE cannot read
// is not an error anywhere: it is a button that draws nothing, on a toolbar, discovered by
// someone looking at the window and wondering where the play button went.
//
// Nothing else catches it. The SVG is data, so it does not fail to compile; the icons are only
// loaded when the editor is constructed, which no other test does; and the failure looks exactly
// like a button that is meant to be blank.
//
// The kit's own KitTests checks this artwork is well-formed. This checks the half that is
// Quarry's: that the constant each button names still exists, that JUCE can parse it, and that
// recolouring it does something -- because the artwork now arrives from another repo through
// tools/sync_okstudio.py, and a sync moves the whole set at once.
//

#ifndef QUARRY_ICON_TEST_H
#define QUARRY_ICON_TEST_H

#include <iostream>
#include <string>
#include <string_view>

#include <JuceHeader.h>

#include <okstudio/Icons.h>

namespace icon_test_utils
{
static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (! condition)
    {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}

struct Icon
{
    const char* name;
    std::string_view svg;
};
} // namespace icon_test_utils

inline bool icon_test()
{
    using icon_test_utils::check;
    using icon_test_utils::Icon;

    icon_test_utils::failures = 0;

    // Every icon the window draws, named the way its button names it. Listed by hand rather than
    // swept out of the kit's namespace, because the point is that each one a button asks for is
    // there: a sweep would pass just as happily over a set that had lost the one the play button
    // wanted, and the kit serves five products with icons Quarry does not use.
    const Icon all[] = {
        {"skipToStart", okstudio::icons::skipToStart},
        {"play", okstudio::icons::play},
        {"pause", okstudio::icons::pause},
        {"record", okstudio::icons::record},
        {"recording", okstudio::icons::recording},
        {"unfoldHorizontal", okstudio::icons::unfoldHorizontal},
        {"foldHorizontal", okstudio::icons::foldHorizontal},
        {"settings", okstudio::icons::settings},
        {"mute", okstudio::icons::mute},
        {"unmute", okstudio::icons::unmute},
        {"trash", okstudio::icons::trash},
        {"folderOpen", okstudio::icons::folderOpen},
    };

    for (const auto& icon : all)
    {
        const auto drawable = juce::Drawable::createFromImageData(icon.svg.data(), icon.svg.size());

        check(drawable != nullptr, std::string(icon.name) + " does not parse: the button would draw nothing");

        if (drawable == nullptr)
            continue;

        // A drawable that parsed but drew nothing -- every path dropped, or a viewBox JUCE read
        // as empty -- is the same blank button by another route.
        const auto bounds = drawable->getDrawableBounds();

        check(! bounds.isEmpty(), std::string(icon.name) + " parses to nothing drawable");
        check(bounds.getWidth() > 1.0f && bounds.getHeight() > 1.0f,
              std::string(icon.name) + " is too small to be a 24 px icon");
    }

    // The recolour every caller does, and the reason it works: the artwork is stroked in black,
    // and quarry::lnf::recolourIcon replaces black with whatever the surface wants. An icon that
    // arrived stroked in something else -- Lucide publishes them as currentColor -- parses,
    // draws, and then sits on the panel at 1.09:1 with nothing reporting it.
    {
        int painted = 0;

        for (const auto& icon : all)
        {
            auto drawable = juce::Drawable::createFromImageData(icon.svg.data(), icon.svg.size());

            if (drawable == nullptr)
                continue;

            const auto before = drawable->createComponentSnapshot(
                drawable->getDrawableBounds().getSmallestIntegerContainer(), false, 1.0f);

            drawable->replaceColour(juce::Colours::black, juce::Colours::red);

            const auto after = drawable->createComponentSnapshot(
                drawable->getDrawableBounds().getSmallestIntegerContainer(), false, 1.0f);

            bool moved = false;

            for (int y = 0; y < before.getHeight() && ! moved; ++y)
                for (int x = 0; x < before.getWidth() && ! moved; ++x)
                    if (before.getPixelAt(x, y) != after.getPixelAt(x, y))
                        moved = true;

            check(moved, std::string(icon.name) + " is not stroked in black, so recolouring it does nothing");

            if (moved)
                ++painted;
        }

        std::cout << "  " << painted << " of " << std::size(all) << " icons recolour from black" << std::endl;
    }

    // The two toggle pairs, which are the only icons here whose meaning depends on the other
    // one. A pair that ended up the same drawing would leave the state resting entirely on
    // colour, which docs/UI.md does not allow of anything this window draws.
    check(okstudio::icons::record != okstudio::icons::recording,
          "the record pair is two drawings, so the armed state is not carried by colour alone");
    check(okstudio::icons::foldHorizontal != okstudio::icons::unfoldHorizontal,
          "the centre pair is two drawings, so the held state is not carried by colour alone");

    if (icon_test_utils::failures == 0)
    {
        std::cout << "  PASSED" << std::endl;
        return true;
    }

    return false;
}

#endif // QUARRY_ICON_TEST_H
