//
// The progress strip's geometry. It fills a strip of toolbar background that used to be empty:
// x 537..966, y 10..38, above the transport row QuarryMainView places at y 43 (see its own
// resized()). Plain ints and no JUCE, so Tests/progress_strip_test.h can check the arithmetic
// without building a window to look at.
//

#ifndef ProgressStripLayout_h
#define ProgressStripLayout_h

/**
 * Left to right: the bar, a gap, the caption, a gap, Cancel. Cancel's width is reserved whether
 * or not it is showing right now (it hides itself when the current job is not cancellable), so
 * the caption never shifts sideways as a job starts or stops being one Cancel can reach.
 */
namespace ProgressStripLayout
{

constexpr int X = 537;
constexpr int Y = 10;
constexpr int WIDTH = 429;
constexpr int HEIGHT = 28;

constexpr int BAR_WIDTH = 160;
constexpr int BAR_HEIGHT = 6;

/** Between the bar and the caption, and again between the caption and Cancel. */
constexpr int GAP = 10;

constexpr int CANCEL_WIDTH = 80;
constexpr int CANCEL_HEIGHT = 22;

/** What is left for the caption once the bar, Cancel and both gaps have had theirs. */
constexpr int captionWidth() { return WIDTH - BAR_WIDTH - GAP - CANCEL_WIDTH - GAP; }

/** Where the strip's bottom edge lands. Has to clear the button row at y 43. */
constexpr int bottom() { return Y + HEIGHT; }

} // namespace ProgressStripLayout

#endif // ProgressStripLayout_h
