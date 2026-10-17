//
// Which text counts as a downloadable link. The same helper gates the clipboard paste and the
// browser drag, so a false positive swallows an ordinary Ctrl+V and a false negative makes a
// link refuse to drop.
//

#ifndef QUARRY_URL_DROP_TEST_H
#define QUARRY_URL_DROP_TEST_H

#include <iostream>
#include <string>

#include "Components/Views/UrlDrop.h"

namespace url_drop_test_utils
{
static int failures = 0;

static void expectUrl(const juce::String& input, const juce::String& expected, const std::string& what)
{
    const auto got = quarry::downloadableUrl(input);

    if (got != expected) {
        std::cout << "  FAILED: " << what << " (got \"" << got.toStdString() << "\", wanted \""
                  << expected.toStdString() << "\")" << std::endl;
        ++failures;
    }
}
} // namespace url_drop_test_utils

inline bool url_drop_test()
{
    using namespace url_drop_test_utils;
    failures = 0;

    const juce::String url = "https://www.youtube.com/watch?v=abc123";

    expectUrl(url, url, "plain URL");
    expectUrl("http://example.com/a.mp3", "http://example.com/a.mp3", "plain http URL");
    expectUrl("  \t" + url + " \r\n", url, "surrounding whitespace");
    expectUrl(url + "\nSome Video Title - YouTube", url, "multi-line drag payload (LF)");
    expectUrl(url + "\r\nSome Video Title", url, "multi-line drag payload (CRLF)");
    expectUrl("HTTPS://Example.com/x", "HTTPS://Example.com/x", "uppercase scheme");
    expectUrl("Http://example.com", "Http://example.com", "mixed-case scheme");
    expectUrl("hello world", {}, "non-URL text");
    expectUrl("https://example.com/a b", {}, "internal whitespace");
    expectUrl("C:\\Users\\me\\take.wav", {}, "Windows file path");
    expectUrl("/home/me/take.wav", {}, "POSIX file path");
    expectUrl("ftp://example.com/a.mp3", {}, "ftp scheme");
    expectUrl("https://", {}, "scheme with no host");
    expectUrl("Title\n" + url, {}, "URL only on second line");
    expectUrl("", {}, "empty");
    expectUrl("   \n  ", {}, "whitespace only");

    std::cout << (failures == 0 ? "  all passed" : "  FAILURES") << std::endl;
    return failures == 0;
}

#endif // QUARRY_URL_DROP_TEST_H
