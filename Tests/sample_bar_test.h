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

#include <okstudio/Obsidian.h>

#include "UIDefines.h"
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
    check(folder + OPEN_GAP + OPEN_BUTTON + MIDDLE_GAP + status == middleWidth(WIDTH),
          "the folder, the Open button and the two gaps are the whole middle");

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
        check(folderWidth(narrow) + OPEN_GAP + OPEN_BUTTON + MIDDLE_GAP + statusWidth(narrow)
                  == middleWidth(narrow),
              "a narrow bar's split still adds up");
        check(folderWidth(narrow) <= folder, "a narrower bar gives the folder no more than a wide one");
    }

    // Wide enough that the folder gets everything it wants and the status gets the surplus.
    {
        const int wide = WIDTH + 400;
        check(folderWidth(wide) == FOLDER_IDEAL, "given room, the folder takes what a path needs and no more");
        check(statusWidth(wide) > status, "the surplus on a wider bar goes to the status");
    }

    // The Open button is a folder glyph on a 24 px grid, and JUCE fits a drawable into whatever
    // box it is given. A box narrower than it is tall would letterbox the icon and put it out of
    // square with every other icon in the window, so the width has to clear the bar's own height.
    check(OPEN_BUTTON >= INNER_HEIGHT, "the Open button is at least square, so its icon is not squashed");
    check(INNER_HEIGHT == HEIGHT - MARGIN_Y * 2, "the inner height is what the inset actually leaves");

    // The picker carries the parameter's own choice names now that the "PITCH BEND" caption is
    // gone, so the longer of the two has to fit the width reserved for it. A ComboBox clips its
    // label rather than shrinking the text, and the arrow takes room off the right, so the
    // measurement is against the width less that. Checked here for the same reason the when line
    // is checked in engine_catalog_test.h: nothing makes the overflow visible except opening the
    // Transcribe page and looking at the bottom of the window.
    {
        // Measured in the font the picker is actually drawn in, which is not the platform
        // default: Obsidian's getComboBoxFont is ui(14.0f), and ui() resolves to whatever the
        // editor handed setUiTypefaces -- Montserrat. Measuring in the default sans, as this
        // first did, was wrong on the typeface and on the size at once, and Montserrat is the
        // wider of the two, so it was wrong in the direction that lets an overflow through.
        okstudio::obsidian::setUiTypefaces(UIDefines::MONTSERRAT_REGULAR(), UIDefines::MONTSERRAT_SEMIBOLD());

        // What JUCE's default LookAndFeel leaves the label after the arrow button and its
        // padding: positionComboBoxText insets by the button width plus a small margin.
        constexpr int arrowAndPadding = 30;

        const auto widest = okstudio::obsidian::ui(14.0f).getStringWidthFloat("Single Pitch Bend");
        const auto room = (float) (PITCH_BEND - arrowAndPadding);

        check(widest <= room, "the longer pitch-bend choice fits its picker unclipped");

        std::cout << "  widest pitch bend choice: " << widest << " px of " << room << std::endl;
    }

    // STATUS_FLOOR is a claim about sentences, so it is checked against the sentences. These are
    // the fixed ones SampleBar writes -- the variable part of a result line is a filename, which
    // elides from the right and still reads. The folder button beside it has no such floor on
    // purpose: what it shows is a path a person chose, of no length at all, which is why it
    // shows the last two components and keeps the rest on its tooltip.
    {
        // The status label carries no font of its own, so it draws in the default sans at the
        // default height -- and the editor points that at Montserrat too. Same correction as
        // the picker above: the platform default is narrower, and measuring against it would
        // have said a sentence fits when the window clips it.
        juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface(UIDefines::MONTSERRAT_REGULAR());

        const auto font = juce::Font(juce::FontOptions(15.0f));

        const char* fixed[] = {
            "Record or drop something to save.",
            "Pick a format.",
            "Saved take_01.wav and take_01.mid.",
        };

        float widest = 0.0f;
        const char* widestText = "";

        for (const auto* sentence : fixed)
        {
            const auto width = font.getStringWidthFloat(sentence);

            if (width > widest)
            {
                widest = width;
                widestText = sentence;
            }
        }

        check(widest <= (float) STATUS_FLOOR, "the status label's fixed sentences fit the floor it keeps");
        std::cout << "  widest fixed status: \"" << widestText << "\" at " << widest << " px of " << STATUS_FLOOR
                  << std::endl;
    }

    if (sample_bar_test_utils::failures == 0)
    {
        std::cout << "  PASSED" << std::endl;
        return true;
    }

    return false;
}

#endif // QUARRY_SAMPLE_BAR_TEST_H
