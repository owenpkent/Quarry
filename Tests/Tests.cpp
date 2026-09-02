#include <JuceHeader.h>

#include <okstudio/Obsidian.h>

#include "UIDefines.h"
#include "Features.h"
#include "BasicPitchCNN.h"
#include <vector>
#include "test_utils.h"
#include "features_test.h"
#include "cnn_test.h"
#include "perf_test.h"
#include "notes_test.h"
#include "key_estimate_test.h"
#include "sampler_test.h"
#include "sidecar_integration_test.h"
#include "focus_ring_test.h"
#include "engine_catalog_test.h"
#include "left_column_test.h"
#include "sample_bar_test.h"
#include "icon_test.h"

int main()
{
    // The window's typefaces, set once for the whole run and never unset.
    //
    // Two of these tests measure strings against a width, and a measurement is only worth
    // anything in the font the string is actually drawn in: Obsidian's ui() resolves to whatever
    // setUiTypefaces was handed, and a Font built with no typeface of its own draws in the
    // default sans. The editor points both at Montserrat, so the tests have to as well.
    //
    // It happens here rather than inside the test that first needed it. Both of those calls are
    // process-wide with no way to scope them to one case, so a test that set them was silently
    // deciding what every test after it in this file would measure -- and a test that measured
    // correctly only because sample_bar_test ran first would start measuring in the platform
    // default the day somebody reordered the list below. That is the same class of error
    // SampleBarLayout's STATUS_FLOOR comment describes, where two of three numbers were wrong
    // and neither was wrong in a way anybody could see.
    okstudio::obsidian::setUiTypefaces(UIDefines::MONTSERRAT_REGULAR(), UIDefines::MONTSERRAT_SEMIBOLD());
    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface(UIDefines::MONTSERRAT_REGULAR());

    int result = 0;

    std::cout << std::endl << "FEATURE TEST" << std::endl;
    result |= !feature_test();

    std::cout << std::endl << "CNN TEST" << std::endl;
    result |= !cnn_test();

    std::cout << std::endl << "PERF TEST" << std::endl;
    result |= !perf_test();

    std::cout << std::endl << "NOTES TEST" << std::endl;
    result |= !notes_test();

    std::cout << std::endl << "KEY ESTIMATE TEST" << std::endl;
    result |= !key_estimate_test();

    std::cout << std::endl << "SAMPLER TEST" << std::endl;
    result |= !sampler_test();

    std::cout << std::endl << "SIDECAR INTEGRATION TEST" << std::endl;
    result |= !sidecar_integration_test();

    std::cout << std::endl << "FOCUS RING TEST" << std::endl;
    result |= !focus_ring_test();

    std::cout << std::endl << "ENGINE CATALOG TEST" << std::endl;
    result |= !engine_catalog_test();

    std::cout << std::endl << "LEFT COLUMN TEST" << std::endl;
    result |= !left_column_test();

    std::cout << std::endl << "SAMPLE BAR TEST" << std::endl;
    result |= !sample_bar_test();

    std::cout << std::endl << "ICON TEST" << std::endl;
    result |= !icon_test();

    return result;
}