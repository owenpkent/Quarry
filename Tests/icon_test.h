//
// Every icon in the window, actually parsed.
//
// The icons are SVG source compiled into BinaryData and turned into a Drawable at runtime by
// juce::Drawable::createFromImageData. That call returns a null pointer when the parse fails, and
// DrawableButton::setImages takes a null pointer without complaint, so an icon JUCE cannot read
// is not an error anywhere: it is a button that draws nothing, on a toolbar, discovered by
// someone looking at the window and wondering where the play button went.
//
// Nothing else catches it. The SVG is data, so it does not fail to compile; the icons are only
// loaded when the editor is constructed, which no other test does; and the failure looks exactly
// like a button that is meant to be blank.
//
// It matters more now than it did. The artwork is Lucide rather than hand-cut, so it arrives from
// outside this repo and is replaced wholesale rather than edited a path at a time, and the whole
// set moves at once when the pin does.
//

#ifndef QUARRY_ICON_TEST_H
#define QUARRY_ICON_TEST_H

#include <iostream>
#include <string>

#include <JuceHeader.h>

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
    const char* data;
    int size;
};
} // namespace icon_test_utils

inline bool icon_test()
{
    using icon_test_utils::check;
    using icon_test_utils::Icon;

    icon_test_utils::failures = 0;

    // Every icon the window draws. Listed by hand rather than swept out of BinaryData, because
    // the point is that each one a button asks for is there: a sweep would pass just as happily
    // over a set that had lost the one the play button wanted.
    const Icon all[] = {
        {"back", BinaryData::back_svg, BinaryData::back_svgSize},
        {"play", BinaryData::play_svg, BinaryData::play_svgSize},
        {"pause", BinaryData::pause_svg, BinaryData::pause_svgSize},
        {"recordingoff", BinaryData::recordingoff_svg, BinaryData::recordingoff_svgSize},
        {"recordingon", BinaryData::recordingon_svg, BinaryData::recordingon_svgSize},
        {"center_off", BinaryData::center_off_svg, BinaryData::center_off_svgSize},
        {"center_on", BinaryData::center_on_svg, BinaryData::center_on_svgSize},
        {"settings", BinaryData::settings_svg, BinaryData::settings_svgSize},
        {"mute", BinaryData::mute_svg, BinaryData::mute_svgSize},
        {"unmute", BinaryData::unmute_svg, BinaryData::unmute_svgSize},
        {"deleteicon", BinaryData::deleteicon_svg, BinaryData::deleteicon_svgSize},
        {"folderopen", BinaryData::folderopen_svg, BinaryData::folderopen_svgSize},
    };

    for (const auto& icon : all)
    {
        const auto drawable = juce::Drawable::createFromImageData(icon.data, (size_t) icon.size);

        check(drawable != nullptr, std::string(icon.name) + ".svg does not parse: the button would draw nothing");

        if (drawable == nullptr)
            continue;

        // A drawable that parsed but drew nothing -- every path dropped, or a viewBox JUCE read
        // as empty -- is the same blank button by another route.
        const auto bounds = drawable->getDrawableBounds();

        check(! bounds.isEmpty(), std::string(icon.name) + ".svg parses to nothing drawable");
        check(bounds.getWidth() > 1.0f && bounds.getHeight() > 1.0f,
              std::string(icon.name) + ".svg is too small to be a 24 px icon");
    }

    // The recolour every caller does, and the reason it works: the artwork is stroked in black,
    // and quarry::lnf::recolourIcon replaces black with whatever the surface wants. An icon that
    // arrived stroked in something else -- Lucide publishes them as currentColor -- parses,
    // draws, and then sits on the panel at 1.09:1 with nothing reporting it.
    {
        int painted = 0;

        for (const auto& icon : all)
        {
            auto drawable = juce::Drawable::createFromImageData(icon.data, (size_t) icon.size);

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

            check(moved, std::string(icon.name) + ".svg is not stroked in black, so recolouring it does nothing");

            if (moved)
                ++painted;
        }

        std::cout << "  " << painted << " of " << std::size(all) << " icons recolour from black" << std::endl;
    }

    if (icon_test_utils::failures == 0)
    {
        std::cout << "  PASSED" << std::endl;
        return true;
    }

    return false;
}

#endif // QUARRY_ICON_TEST_H
