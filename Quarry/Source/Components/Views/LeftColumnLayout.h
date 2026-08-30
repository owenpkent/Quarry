//
// The geometry of the Transcribe page's left column, in one place, because four files have to
// agree about it and no two of them can see each other.
//

#ifndef LeftColumnLayout_h
#define LeftColumnLayout_h

/**
 * Where the column of sections sits, and how tall each section stands in each of its states.
 *
 * The column used to be three setBounds calls with the numbers typed straight in, which worked
 * exactly as long as no section could change height. Two of them now collapse to their label row
 * when their toggle is off, and the one above them grows when ADVANCED opens, so the stack is
 * computed and the total has to be checked: the tall case, everything expanded at once, is the
 * one nobody assembles by hand and the one that runs off the bottom of the window.
 *
 * Each view keeps its own internal grid and ties it to the number here with a static_assert, so
 * a control moved inside a panel either updates this or fails to compile. Plain ints and no JUCE,
 * so the arithmetic can be checked in Tests without building a window to look at.
 */
namespace LeftColumnLayout
{

constexpr int X = 29;
constexpr int TOP = 140;
constexpr int WIDTH = 274;

/** Between one section's bottom edge and the next section's label row. */
constexpr int GAP = 14;

/** Where the footer starts. The column has to clear it. */
constexpr int SAMPLE_BAR_TOP = 665;

constexpr int BOTTOM_LIMIT = SAMPLE_BAR_TOP - GAP;

/** A section showing its label row and nothing else. Matches UIDefines' LEFT_SECTIONS_TOP_PAD,
    which is the space every section already reserves at its top for that row. */
constexpr int COLLAPSED = 24;

/** MODEL, which never collapses: there is no state of this app in which no model runs. Three
    heights, because the built-in engine owns three decoder rotaries that a sidecar engine has
    no use for, and ADVANCED folds them away. */
constexpr int MODEL_SIDECAR_ENGINE = 104;
constexpr int MODEL_ADVANCED_CLOSED = 128;
constexpr int MODEL_ADVANCED_OPEN = 223;

/** How wide MODEL's picker is, and with it the two lines of text under it.
    Here rather than inside the panel because the engine catalog's own copy is measured against
    it (Tests/engine_catalog_test.h): those lines are drawn into this width and clipped, not
    wrapped, and a sentence one word too long for it is invisible until someone selects that
    engine and looks. */
constexpr int MODEL_ROW_WIDTH = 238;

constexpr int SCALE_QUANTIZE_EXPANDED = 134;
constexpr int TIME_QUANTIZE_EXPANDED = 120;

/** Where the bottom edge of the last section lands, for three given heights. */
constexpr int stackBottom(int inFirst, int inSecond, int inThird)
{ return TOP + inFirst + GAP + inSecond + GAP + inThird; }

/** Everything expanded at once: MODEL with its rotaries out, and both quantize sections on. */
constexpr int tallestStackBottom()
{ return stackBottom(MODEL_ADVANCED_OPEN, SCALE_QUANTIZE_EXPANDED, TIME_QUANTIZE_EXPANDED); }

/** The state the page opens in: built-in engine, ADVANCED closed, neither quantize section on. */
constexpr int defaultStackBottom()
{ return stackBottom(MODEL_ADVANCED_CLOSED, COLLAPSED, COLLAPSED); }

} // namespace LeftColumnLayout

#endif // LeftColumnLayout_h
