#include <JuceHeader.h>
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

int main()
{
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

    return result;
}