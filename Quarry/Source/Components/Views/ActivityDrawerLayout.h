//
// The activity drawer's geometry, in one place because it floats over the left column and the
// numbers that place it live in two other files.
//

#ifndef ActivityDrawerLayout_h
#define ActivityDrawerLayout_h

#include "LeftColumnLayout.h"
#include "SampleBarLayout.h"

/**
 * Where the activity drawer sits and how it divides its own height.
 *
 * There is no fourth slot in the left column for it -- LeftColumnLayout's own comment says the
 * tall case already runs to BOTTOM_LIMIT -- so the drawer is an overlay rather than a section:
 * hidden until backtick opens it, and drawn on top of whatever the column and the footer are
 * showing rather than beside them. Its footprint borrows the column's left edge and the sample
 * bar's width, because between them those are the only two widths this part of the window has
 * already agreed on, and its bottom edge is the same BOTTOM_LIMIT the column stops short of.
 *
 * Plain ints and no JUCE, so Tests/activity_drawer_test.h can check the arithmetic without
 * building a window to look at.
 */
namespace ActivityDrawerLayout
{

constexpr int X = LeftColumnLayout::X;
constexpr int WIDTH = SampleBarLayout::WIDTH;
constexpr int BOTTOM = LeftColumnLayout::BOTTOM_LIMIT;

constexpr int HEIGHT = 300;

/** The title row: "ACTIVITY", the status text, and the close button. */
constexpr int HEADER = 24;

/** The prompt row: the painted chevron prefix and the single-line command editor. */
constexpr int PROMPT = 24;

/** One gap: above the header, between the header and the log, between the log and the prompt,
    and below the prompt. */
constexpr int PAD = 8;

/** How many of those gaps the drawer actually has, so the arithmetic below and the test that
    checks it are reading the same number rather than two copies of 4. */
constexpr int PAD_COUNT = 4;

/** Where the drawer's top edge lands, given its fixed bottom and its own height. */
constexpr int top() { return BOTTOM - HEIGHT; }

/** What is left for the scrolling log once the header, the prompt and every gap between them
    have had theirs. */
constexpr int logHeight() { return HEIGHT - HEADER - PROMPT - PAD_COUNT * PAD; }

} // namespace ActivityDrawerLayout

#endif // ActivityDrawerLayout_h
