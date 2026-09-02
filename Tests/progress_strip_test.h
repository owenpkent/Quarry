//
// The progress strip fills a strip of toolbar background above the transport row (see
// ProgressStripLayout.h's own note), so its arithmetic is checked the same way
// ActivityDrawerLayout's is: plain ints, no window required.
//

#ifndef QUARRY_PROGRESS_STRIP_TEST_H
#define QUARRY_PROGRESS_STRIP_TEST_H

#include <iostream>
#include <string>

#include "Components/Views/ProgressStripLayout.h"

namespace progress_strip_test_utils
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
} // namespace progress_strip_test_utils

inline bool progress_strip_test()
{
    using namespace progress_strip_test_utils;
    namespace P = ProgressStripLayout;

    failures = 0;

    check(P::bottom() <= 43, "the strip's bottom edge clears the button row at y 43");

    check(P::X + P::WIDTH == 966, "the strip's right edge lands where the button row ends");

    check(P::captionWidth() >= 120,
          "the caption keeps enough room to say something once the bar and Cancel have theirs");

    if (failures == 0)
        std::cout << "  PASSED" << std::endl;

    return failures == 0;
}

#endif // QUARRY_PROGRESS_STRIP_TEST_H
