//
// The footer's width arithmetic, which nobody was checking.
//
// The left column has left_column_test.h because three panels stack against a number none of
// them can see. The sample bar has the same problem lying down: everything in it competes for
// one fixed width, and the two controls that stretch -- the folder path and the status message
// -- silently absorb whatever any new control takes. That already happened once. A pitch-bend
// picker and its caption took 198 px off the right, and an incidental `jmin(300, width / 2)`
// handed the loss to the middle, dropping the status label from 307 px to 199 and truncating the
// one sentence that says which take was written and where. Nothing failed, nothing warned, and
// the only way to find out was to save something and look.
//
// So the split is stated in SampleBarLayout and checked here.
//

#ifndef QUARRY_SAMPLE_BAR_TEST_H
#define QUARRY_SAMPLE_BAR_TEST_H

#include <iostream>
#include <string>

#include <JuceHeader.h>

#include "Components/Views/SampleBarLayout.h"

namespace sample_bar_test_utils
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
} // namespace sample_bar_test_utils

inline bool sample_bar_test()
{
    using sample_bar_test_utils::check;
    using namespace SampleBarLayout;

    sample_bar_test_utils::failures = 0;

    // The shipped bar, which is the case that matters: it is the only width this ever has.
    const int folder = folderWidth(WIDTH);
    const int status = statusWidth(WIDTH);

    check(folder >= FOLDER_FLOOR, "the folder button keeps its floor at the shipped width");
    check(status >= STATUS_FLOOR, "the status label keeps its floor at the shipped width");
    check(folder <= FOLDER_IDEAL, "the folder button never exceeds what a path needs");

    // The two of them plus the gap are the middle exactly. A split that does not add up leaves a
    // strip of panel showing between them, or runs the status off the end of the bar.
    check(folder + MIDDLE_GAP + status == middleWidth(WIDTH),
          "the folder, the gap and the status are the whole middle");

    // What the regression looked like, as a number rather than as a description. The status is
    // the half with nowhere else to say what it says, so it is the half that keeps its room.
    check(status > folder, "the status label, which nothing else repeats, gets the larger share");

    // Room has to exist before it can be divided. A middle that has gone negative means the
    // fixed right-hand run has eaten the whole bar, which is the failure this file exists to
    // catch on the next control somebody adds.
    check(middleWidth(WIDTH) > 0, "the fixed controls leave a middle to divide");

    // Narrow enough that both floors cannot be met at once: the folder gives way to its own
    // floor and the status takes the rest, and neither goes negative.
    {
        const int narrow = WIDTH - 200;
        check(folderWidth(narrow) >= 0 && statusWidth(narrow) >= 0,
              "a bar too narrow for both floors still divides into two real widths");
        check(folderWidth(narrow) + MIDDLE_GAP + statusWidth(narrow) == middleWidth(narrow),
              "a narrow bar's split still adds up");
        check(folderWidth(narrow) <= folder, "a narrower bar gives the folder no more than a wide one");
    }

    // Wide enough that the folder gets everything it wants and the status gets the surplus.
    {
        const int wide = WIDTH + 400;
        check(folderWidth(wide) == FOLDER_IDEAL, "given room, the folder takes what a path needs and no more");
        check(statusWidth(wide) > status, "the surplus on a wider bar goes to the status");
    }

    // The picker carries the parameter's own choice names now that the "PITCH BEND" caption is
    // gone, so the longer of the two has to fit the width reserved for it. A ComboBox clips its
    // label rather than shrinking the text, and the arrow takes room off the right, so the
    // measurement is against the width less that. Checked here for the same reason the when line
    // is checked in engine_catalog_test.h: nothing makes the overflow visible except opening the
    // Transcribe page and looking at the bottom of the window.
    {
        // What JUCE's default LookAndFeel leaves the label after the arrow button and its
        // padding: positionComboBoxText insets by the button width plus a small margin.
        constexpr int arrowAndPadding = 30;

        const auto font = juce::Font(juce::FontOptions(15.0f));
        const auto widest = font.getStringWidthFloat("Single Pitch Bend");
        const auto room = (float) (PITCH_BEND - arrowAndPadding);

        check(widest <= room, "the longer pitch-bend choice fits its picker unclipped");

        std::cout << "  widest pitch bend choice: " << widest << " px of " << room << std::endl;
    }

    if (sample_bar_test_utils::failures == 0)
    {
        std::cout << "  PASSED" << std::endl;
        return true;
    }

    return false;
}

#endif // QUARRY_SAMPLE_BAR_TEST_H
