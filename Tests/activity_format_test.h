//
// formatActivityLine's whole job is "HH:MM:SS  text" in local time, so the thing worth getting
// wrong is the timezone. A hard-coded expected string would pass or fail depending on where and
// when the build runs; this checks a fixed instant against juce::Time's own reading of it
// instead, so the test means the same thing everywhere.
//

#ifndef QUARRY_ACTIVITY_FORMAT_TEST_H
#define QUARRY_ACTIVITY_FORMAT_TEST_H

#include <iostream>
#include <string>

#include <JuceHeader.h>

#include "ActivityLog.h"
#include "Components/Views/ActivityFormat.h"

namespace activity_format_test_utils
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
} // namespace activity_format_test_utils

inline bool activity_format_test()
{
    using namespace activity_format_test_utils;

    failures = 0;

    quarry::ActivityLine line;
    line.kind = quarry::ActivityLine::Kind::Quarry;
    // Any fixed instant does, since the expectation below is built from the same instant
    // rather than typed in.
    line.timeMs = 1700000000000LL;
    line.seq = 1;
    line.text = "sidecar ready";

    const auto formatted = quarry::formatActivityLine(line);

    const juce::Time time(line.timeMs);
    const auto pad = [](int n) { return juce::String(n).paddedLeft('0', 2); };
    const auto expected = pad(time.getHours()) + ":" + pad(time.getMinutes()) + ":" + pad(time.getSeconds())
                         + "  " + line.text;

    check(formatted == expected, "HH:MM:SS, two spaces, then the text, in whatever timezone this runs in");
    check(formatted.endsWith(line.text), "the text itself passes through untouched");

    if (failures == 0)
        std::cout << "  PASSED" << std::endl;

    return failures == 0;
}

#endif // QUARRY_ACTIVITY_FORMAT_TEST_H
