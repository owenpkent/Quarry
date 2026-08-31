//
// The Transcribe page's left column now has states, and one of them runs off the bottom.
//
// It used to be three setBounds calls with the coordinates typed in, and a wrong number there
// was visible the moment anyone looked at the window. That is no longer true. Two of the three
// sections collapse to their label row when their toggle is off, which is how they both start,
// and MODEL grows by ninety pixels when ADVANCED opens. The tall case is every one of those
// expanded at once, and it is the case nobody assembles by hand: you would have to turn on
// scale quantization, turn on time quantization, and open ADVANCED, in one session, and then
// look at the bottom of the column.
//
// The three heights live in three different files, none of which can see the other two or the
// sample bar they all have to clear. So the arithmetic is checked here instead of being found
// later by someone whose FORCE slider is behind the footer.
//

#ifndef QUARRY_LEFT_COLUMN_TEST_H
#define QUARRY_LEFT_COLUMN_TEST_H

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "Components/Views/LeftColumnLayout.h"

namespace left_column_test_utils
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
} // namespace left_column_test_utils

inline bool left_column_test()
{
    using namespace left_column_test_utils;
    namespace L = LeftColumnLayout;

    failures = 0;

    check(L::BOTTOM_LIMIT < L::SAMPLE_BAR_TOP, "the column stops short of the footer");
    check(L::COLLAPSED == 24, "a collapsed section is one label row, the same 24 px every panel reserves");

    // Every state the column can be in, not just the two worth naming. MODEL has three heights
    // and each quantize section has two, so there are twelve, and the one that overflows is not
    // necessarily the one anybody thought to check.
    const std::vector<int> model {L::MODEL_SIDECAR_ENGINE, L::MODEL_ADVANCED_CLOSED, L::MODEL_ADVANCED_OPEN};
    const std::vector<int> scale {L::COLLAPSED, L::SCALE_QUANTIZE_EXPANDED};
    const std::vector<int> time {L::COLLAPSED, L::TIME_QUANTIZE_EXPANDED};

    int checked = 0;
    int worst = 0;

    for (const auto m : model)
        for (const auto s : scale)
            for (const auto t : time)
            {
                const auto bottom = L::stackBottom(m, s, t);
                worst = std::max(worst, bottom);
                ++checked;

                check(bottom <= L::BOTTOM_LIMIT, "a left column state fits above the footer");
            }

    check(checked == 12, "every combination of section states was checked");
    check(worst == L::tallestStackBottom(), "the tall case really is everything expanded at once");

    // The reason any of this exists. The page opens with the built-in engine, ADVANCED closed
    // and neither quantize section on, and in that state the column used to hold 476 px of
    // controls, two thirds of which did nothing until someone switched them on. If a change
    // ever puts that back, the default state is where it will show up first.
    check(L::defaultStackBottom() - L::TOP <= 250,
          "the column the page opens with is a fraction of the window, not two thirds of it");

    // Collapsing has to buy something. If the default ever costs as much as the tall case, the
    // sections are not collapsing at all and the toggle is decorative.
    check(L::defaultStackBottom() < L::tallestStackBottom() - 200,
          "collapsing the unused sections is worth more than 200 px");

    std::cout << "  default column: " << (L::defaultStackBottom() - L::TOP) << " px, tallest: "
              << (L::tallestStackBottom() - L::TOP) << " px, room: " << (L::BOTTOM_LIMIT - L::TOP) << " px"
              << std::endl;

    if (failures == 0)
        std::cout << "  all left column checks passed" << std::endl;

    return failures == 0;
}

#endif // QUARRY_LEFT_COLUMN_TEST_H
