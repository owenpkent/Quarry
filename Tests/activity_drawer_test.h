//
// The activity drawer has no window to open in a unit test, but its geometry is plain ints and
// can be checked the same way LeftColumnLayout and SampleBarLayout are: the drawer's bottom
// edge has to land on BOTTOM_LIMIT, it has to clear the top of the column it floats over, and
// its own rows and gaps have to add up to the height it was given.
//

#ifndef QUARRY_ACTIVITY_DRAWER_TEST_H
#define QUARRY_ACTIVITY_DRAWER_TEST_H

#include <iostream>
#include <string>

#include "Components/Views/ActivityDrawerLayout.h"
#include "Components/Views/LeftColumnLayout.h"

namespace activity_drawer_test_utils
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
} // namespace activity_drawer_test_utils

inline bool activity_drawer_test()
{
    using namespace activity_drawer_test_utils;
    namespace A = ActivityDrawerLayout;

    failures = 0;

    check(A::top() + A::HEIGHT == LeftColumnLayout::BOTTOM_LIMIT,
          "the drawer's bottom edge sits on the same limit the left column stops short of");

    check(A::top() > LeftColumnLayout::TOP, "the drawer opens below the top of the left column");

    check(A::HEADER + A::logHeight() + A::PROMPT + A::PAD_COUNT * A::PAD == A::HEIGHT,
          "the header, the log, the prompt and their gaps add up to the whole drawer");

    if (failures == 0)
        std::cout << "  PASSED" << std::endl;

    return failures == 0;
}

#endif // QUARRY_ACTIVITY_DRAWER_TEST_H
