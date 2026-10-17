//
// Opt-in: the updater's manual-redirect download against a real GitHub release asset. It needs
// the network, so it runs only with QUARRY_NET_TESTS=1 and only on Windows, where the code is.
// github.com answers with a redirect to a signed release-assets URL, which is the chain a
// Quarry release download takes.
//

#ifndef QUARRY_UPDATE_NETWORK_TEST_H
#define QUARRY_UPDATE_NETWORK_TEST_H

#include <iostream>

#include <JuceHeader.h>

#if JUCE_WINDOWS
    #include "Components/UpdateInstaller.h"

namespace update_network_test_utils
{
// Prints the installer's own log lines, which carry each hop's host and status.
struct StdoutLogger final : public juce::Logger
{
    void logMessage(const juce::String& message) override { std::cout << "  " << message << std::endl; }
};
} // namespace update_network_test_utils
#endif

inline bool update_network_test()
{
#if JUCE_WINDOWS
    if (juce::SystemStats::getEnvironmentVariable("QUARRY_NET_TESTS", {}) != "1") {
        std::cout << "  skipped (set QUARRY_NET_TESTS=1)" << std::endl;
        return true;
    }

    using quarry::update::UpdateInstaller;
    update_network_test_utils::StdoutLogger logger;
    juce::Logger::setCurrentLogger(&logger);

    bool ok = true;
    const juce::String url = "https://github.com/cli/cli/releases/download/v2.60.0/gh_2.60.0_checksums.txt";

    juce::String text, error;
    const bool fetched = UpdateInstaller::fetchSmallText(url, 16 * 1024, text, error);
    if (!fetched || text.isEmpty() || !text.contains("gh_2.60.0_windows_amd64.zip")) {
        std::cout << "  FAILED: fetch with a 16 KB cap (" << error.toStdString() << ", " << text.length()
                  << " chars)" << std::endl;
        ok = false;
    }

    juce::String small_text, small_error;
    if (UpdateInstaller::fetchSmallText(url, 64, small_text, small_error)) {
        std::cout << "  FAILED: a 64 byte cap should refuse the file" << std::endl;
        ok = false;
    }

    juce::Logger::setCurrentLogger(nullptr);
    std::cout << (ok ? "  all passed" : "  FAILURES") << std::endl;
    return ok;
#else
    return true;
#endif
}

#endif // QUARRY_UPDATE_NETWORK_TEST_H
