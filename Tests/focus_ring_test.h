//
// Checking that the focus ring is on the screen, which is not the same question as whether
// its colour clears 3:1.
//
// This test exists because of a bug the contrast checker could not have found. The ring was
// drawn by expanding outwards from the control's paint rect, which is the local bounds less
// a pixel, so a 2px stroke landed one to three pixels beyond the component. JUCE clips a
// component's painting to its own bounds unless it calls setPaintingIsUnclipped, which
// nothing in Quarry does, so every straight edge of the ring was clipped away and only
// fragments of the rounded corners survived: 72 pixels of 632, which this test prints.
// Every control was focusable, every one drew a ring, and what reached the screen was four
// specks in the corners.
//
// That ratio is why the first check below is not enough on its own. "Draws something" was
// true of the broken ring. "Nothing falls outside the control" is the check that fails.
//
// The palette was correct the whole time. tools/contrast_check.py measured the accent
// against the control at 6.88:1 and passed, because a ratio is a fact about two colours and
// says nothing about whether either of them reaches a pixel. That gap is what this closes:
// the checker owns the values, and this owns the geometry.
//

#ifndef QUARRY_FOCUS_RING_TEST_H
#define QUARRY_FOCUS_RING_TEST_H

#include <iostream>
#include <string>

#include <JuceHeader.h>

#include "QuarryLookAndFeel.h"

namespace focus_ring_test_utils
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

/** Pixels that are not the background, which for a ring on a plain ground is the ring. */
static int painted(const juce::Image& inImage, juce::Colour inGround)
{
    int count = 0;

    for (int y = 0; y < inImage.getHeight(); ++y)
        for (int x = 0; x < inImage.getWidth(); ++x)
            if (inImage.getPixelAt(x, y) != inGround)
                ++count;

    return count;
}

/** The same, inside one band of an image, so "is there a ring along the top edge" is a
    question that can be asked rather than inferred from a total. */
static int paintedIn(const juce::Image& inImage, juce::Rectangle<int> inBand, juce::Colour inGround)
{
    int count = 0;

    for (int y = inBand.getY(); y < inBand.getBottom(); ++y)
        for (int x = inBand.getX(); x < inBand.getRight(); ++x)
            if (inImage.getPixelAt(x, y) != inGround)
                ++count;

    return count;
}
} // namespace focus_ring_test_utils

inline bool focus_ring_test()
{
    using namespace juce;
    using namespace focus_ring_test_utils;

    failures = 0;

    const int width = 120, height = 28;
    const auto accent = okstudio::obsidian::cyanAccent.base;
    const auto ground = Colours::black;

    // Exactly what QuarryLookAndFeel::drawButtonBackground hands over. Passing the same rect
    // the product passes is the point: a ring that only draws for a rect nobody uses is the
    // bug this is here to catch, in a different costume.
    const auto paintRect = Rectangle<int>(0, 0, width, height).toFloat().reduced(1.0f);

    // As JUCE renders a component: into a surface the size of its bounds, clipped to them.
    Image clipped(Image::ARGB, width, height, true);
    {
        Graphics g(clipped);
        g.fillAll(ground);
        g.reduceClipRegion(Rectangle<int>(0, 0, width, height));
        quarry::lnf::focusRing(g, paintRect, okstudio::obsidian::radius, accent);
    }

    // The same ring with room on every side, which is what it would be if nothing clipped
    // it. Any difference between the two is a part of the ring the user never sees.
    const int pad = 8;
    Image roomy(Image::ARGB, width + pad * 2, height + pad * 2, true);
    {
        Graphics g(roomy);
        g.fillAll(ground);
        g.addTransform(AffineTransform::translation((float) pad, (float) pad));
        quarry::lnf::focusRing(g, paintRect, okstudio::obsidian::radius, accent);
    }

    const auto drawn = painted(clipped, ground);
    const auto whole = painted(roomy, ground);

    std::cout << "  ring pixels: " << drawn << " clipped, " << whole << " unclipped" << std::endl;

    // The bug itself: this was zero.
    check(drawn > 0, "the focus ring draws something inside the control");

    // And the general form of it, which survives a change to either side. An integer
    // translation rasterises identically, so anything short here is clipping.
    check(drawn == whole, "no part of the focus ring falls outside the control");

    // Present on all four edges, so a ring cropped down one side is a failure rather than a
    // slightly smaller number that still passes.
    const int band = 5;
    check(paintedIn(clipped, {0, 0, width, band}, ground) > 0, "ring along the top edge");
    check(paintedIn(clipped, {0, height - band, width, band}, ground) > 0, "ring along the bottom edge");
    check(paintedIn(clipped, {0, 0, band, height}, ground) > 0, "ring along the left edge");
    check(paintedIn(clipped, {width - band, 0, band, height}, ground) > 0, "ring along the right edge");

    // The middle of a focused control belongs to the control. A ring that filled it would
    // pass every check above and still be wrong.
    const auto middle = Rectangle<int>(0, 0, width, height).reduced(band + 2);
    check(paintedIn(clipped, middle, ground) == 0, "the ring is a ring, not a fill");

    if (failures == 0)
        std::cout << "  all focus ring checks passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_FOCUS_RING_TEST_H
