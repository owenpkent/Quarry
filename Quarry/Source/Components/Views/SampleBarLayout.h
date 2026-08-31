//
// The footer's horizontal budget, in one place, because everything in it is competing for the
// same 941 pixels and nothing was writing down who wins.
//

#ifndef SampleBarLayout_h
#define SampleBarLayout_h

/**
 * How the sample bar divides its width.
 *
 * The bar is a fixed width in a fixed window, so every control added to it is taken from the two
 * that stretch: the folder path on the left of the middle and the status message on the right of
 * it. That went unnoticed once already. The pitch-bend picker arrived with a painted label and
 * two gaps, took 198 px off the right-hand side, and the middle absorbed all of it through an
 * incidental `jmin(300, width / 2)` -- the folder fell from 300 px to 210 and the status from
 * 307 to 199, quietly truncating the sentence that says which take was written and where.
 *
 * So the split is written down rather than falling out of an expression, and the priority is
 * stated: when there is not enough for both, the folder gives way. Its full path is on its own
 * tooltip and one click away in the chooser, while the status sentence exists nowhere else in
 * the window at all. Plain ints and no JUCE, so Tests/sample_bar_test.h can check the arithmetic
 * without building a window to look at.
 */
namespace SampleBarLayout
{

/** As QuarryMainView places it. */
constexpr int X = 29;
constexpr int WIDTH = 941;
constexpr int HEIGHT = 46;

/** What resized() insets by before laying anything out. */
constexpr int MARGIN_X = 10;
constexpr int MARGIN_Y = 8;

/** The "SAVE TO" caption, painted rather than laid out, and the room reserved for it. */
constexpr int SAVE_TO_LABEL = 74;

// The right-hand run, in the order resized() removes it.
constexpr int SAVE_BUTTON = 78;
constexpr int SAVE_GAP = 10;
constexpr int MIDI_TOGGLE = 66;
constexpr int WAV_TOGGLE = 64;
constexpr int PITCH_BEND_GAP = 14;

/** Wide enough for "Single Pitch Bend" and its arrow. The picker carries the parameter's own
    choice names rather than shortened ones under a separate "PITCH BEND" caption: the caption
    cost 80 px of a bar that had none to spare, and a control whose two values are "No Pitch
    Bend" and "Single Pitch Bend" has already said what it is. */
constexpr int PITCH_BEND = 150;
constexpr int PITCH_BEND_TRAIL = 10;

/** Between the folder button and the status label. */
constexpr int MIDDLE_GAP = 12;

/** The folder would take this much for a long path, and settles for less when the status needs
    it. Never below FOLDER_FLOOR, at which point it is showing a drive letter and an ellipsis and
    is still a button you can click. */
constexpr int FOLDER_IDEAL = 300;
constexpr int FOLDER_FLOOR = 140;

/** What the status keeps whatever else happens. "Saved take_01.wav and take_01.mid." is the
    sentence this has to fit, and it is the only place that sentence appears. */
constexpr int STATUS_FLOOR = 230;

/** What is left for the folder and the status together, once the fixed controls have had theirs. */
constexpr int middleWidth(int inBarWidth)
{
    return inBarWidth - MARGIN_X * 2 - SAVE_TO_LABEL - SAVE_BUTTON - SAVE_GAP - MIDI_TOGGLE - WAV_TOGGLE
           - PITCH_BEND_GAP - PITCH_BEND - PITCH_BEND_TRAIL;
}

/** The middle minus the gap down it: what the two stretching controls actually share. */
constexpr int shareableWidth(int inBarWidth)
{ return middleWidth(inBarWidth) - MIDDLE_GAP; }

constexpr int folderWidth(int inBarWidth)
{
    const int shareable = shareableWidth(inBarWidth);
    const int afterStatus = shareable - STATUS_FLOOR;

    // Bottom clamp before the top one, and the floor itself clamped to what exists: on a bar too
    // narrow for either floor there is no allocation that satisfies both, and the folder taking
    // its floor out of a smaller middle would give the status a negative width.
    if (afterStatus < FOLDER_FLOOR)
        return shareable < FOLDER_FLOOR ? shareable : FOLDER_FLOOR;

    return afterStatus > FOLDER_IDEAL ? FOLDER_IDEAL : afterStatus;
}

constexpr int statusWidth(int inBarWidth)
{ return shareableWidth(inBarWidth) - folderWidth(inBarWidth); }

} // namespace SampleBarLayout

#endif // SampleBarLayout_h
