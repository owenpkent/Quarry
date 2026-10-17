//
// The updater's rules, which are the part of it that can be wrong without anything crashing: an
// older release offered as an update, a lookalike host accepted as GitHub, a checksum file read
// more loosely than it was written. Everything here is pure string logic from UpdatePolicy.
//

#ifndef QUARRY_UPDATE_POLICY_TEST_H
#define QUARRY_UPDATE_POLICY_TEST_H

#include <iostream>
#include <string>

#include "Components/UpdatePolicy.h"

namespace update_policy_test_utils
{
static int failures = 0;

static void expect(bool condition, const std::string& what)
{
    if (!condition) {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}

static const juce::String kHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const juce::String kName = "QuarrySetup-1.2.3.exe";

static juce::var asset(const juce::String& name, const juce::String& url)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("name", name);
    obj->setProperty("browser_download_url", url);
    return juce::var(obj);
}

static juce::var release(const juce::String& tag, const juce::Array<juce::var>& assets, const juce::String& body = {})
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("tag_name", tag);
    obj->setProperty("body", body);
    obj->setProperty("assets", assets);
    return juce::var(obj);
}
} // namespace update_policy_test_utils

inline bool update_policy_test()
{
    using namespace update_policy_test_utils;
    using namespace quarry::update;
    failures = 0;

    // Version comparison: strictly greater only.
    expect(isNewer("v1.2.0", "1.1.0"), "minor bump is newer");
    expect(isNewer("v1.1.1", "1.1.0"), "patch bump is newer");
    expect(isNewer("v2.0.0", "1.9.9"), "major bump is newer");
    expect(isNewer("v1.10.0", "1.9.0"), "numeric, not textual, comparison");
    expect(!isNewer("v1.1.0", "1.1.0"), "equal is not newer");
    expect(!isNewer("v1.0.9", "1.1.0"), "older is not newer");
    expect(!isNewer("v0.9.0", "1.1.0"), "older major is not newer");

    // Malformed on either side is never an update.
    expect(!isNewer("1.2.0", "1.1.0"), "tag without v");
    expect(!isNewer("v1.2", "1.1.0"), "two components");
    expect(!isNewer("v1.2.3.4", "1.1.0"), "four components");
    expect(!isNewer("v1.2.3-beta", "1.1.0"), "prerelease suffix");
    expect(!isNewer("v1.2.3+build", "1.1.0"), "build suffix");
    expect(!isNewer("v1.02.3", "1.1.0"), "leading zero");
    expect(!isNewer("v1..3", "1.1.0"), "empty component");
    expect(!isNewer("v1.2.3.", "1.1.0"), "trailing dot");
    expect(!isNewer("v-1.2.3", "1.1.0"), "negative");
    expect(!isNewer("v1.2.99999999999", "1.1.0"), "component too long");
    expect(!isNewer("", "1.1.0"), "empty tag");
    expect(!isNewer("v1.2.0", ""), "empty current version");
    expect(!isNewer("v1.2.0", "garbage"), "malformed current version");
    expect(!isNewer("vv1.2.0", "1.1.0"), "double v");

    // Asset selection.
    {
        const juce::String base = "https://github.com/owenpkent/Quarry/releases/download/v1.2.3/";
        const auto info = parseRelease(release("v1.2.3",
                                               {asset("QuarrySetup-1.2.3.exe", base + "QuarrySetup-1.2.3.exe"),
                                                asset("QuarrySetup-1.2.3.exe.sha256", base + "QuarrySetup-1.2.3.exe.sha256"),
                                                asset("sbom.json", base + "sbom.json")},
                                               "Faster transcription"));
        expect(info.has_value(), "release parses");
        if (info) {
            expect(info->version == "1.2.3" && info->tag == "v1.2.3", "version and tag");
            expect(info->installerUrl == base + "QuarrySetup-1.2.3.exe", "installer url");
            expect(info->checksumUrl == base + "QuarrySetup-1.2.3.exe.sha256", "checksum url");
            expect(info->hasInstallableAssets(), "installable");
            expect(info->notes == "Faster transcription", "notes kept");
        }

        const auto no_hash = parseRelease(release("v1.2.3", {asset("QuarrySetup-1.2.3.exe", base + "QuarrySetup-1.2.3.exe")}));
        expect(no_hash && !no_hash->hasInstallableAssets(), "no checksum asset means not installable");

        const auto wrong_version =
            parseRelease(release("v1.2.3",
                                 {asset("QuarrySetup-1.2.2.exe", base + "QuarrySetup-1.2.2.exe"),
                                  asset("QuarrySetup-1.2.2.exe.sha256", base + "QuarrySetup-1.2.2.exe.sha256")}));
        expect(wrong_version && !wrong_version->hasInstallableAssets(), "assets of another version are ignored");

        const auto suffix = parseRelease(release("v1.2.3",
                                                 {asset("evil-QuarrySetup-1.2.3.exe", base + "x.exe"),
                                                  asset("QuarrySetup-1.2.3.exe.sha256.exe", base + "y.exe")}));
        expect(suffix && !suffix->hasInstallableAssets(), "names match exactly, not by suffix");

        const auto off_host = parseRelease(
            release("v1.2.3",
                    {asset("QuarrySetup-1.2.3.exe", "https://evil.example/QuarrySetup-1.2.3.exe"),
                     asset("QuarrySetup-1.2.3.exe.sha256", base + "QuarrySetup-1.2.3.exe.sha256")}));
        expect(off_host && !off_host->hasInstallableAssets(), "asset on a foreign host is dropped");

        expect(!parseRelease(release("v1.2.3-rc1", {})).has_value(), "prerelease tag rejected");
        expect(!parseRelease(release("latest", {})).has_value(), "non-version tag rejected");
        expect(!parseRelease(juce::var("text")).has_value(), "non-object rejected");

        auto* draft = new juce::DynamicObject();
        draft->setProperty("tag_name", "v1.2.3");
        draft->setProperty("draft", true);
        expect(!parseRelease(juce::var(draft)).has_value(), "draft rejected");

        auto* pre = new juce::DynamicObject();
        pre->setProperty("tag_name", "v1.2.3");
        pre->setProperty("prerelease", true);
        expect(!parseRelease(juce::var(pre)).has_value(), "prerelease flag rejected");
    }

    // The download URL allowlist.
    expect(isSafeDownloadUrl("https://github.com/owenpkent/Quarry/releases/download/v1.2.3/a.exe"), "github.com");
    expect(isSafeDownloadUrl("https://objects.githubusercontent.com/x?y=1"), "objects host");
    expect(isSafeDownloadUrl("https://release-assets.githubusercontent.com/x"), "release-assets host");
    expect(isSafeDownloadUrl("HTTPS://GitHub.com/x"), "scheme and host are case-insensitive");
    expect(isSafeDownloadUrl("https://github.com:443/x"), "explicit default port");
    expect(isSafeDownloadUrl("https://github.com"), "no path");
    expect(!isSafeDownloadUrl("http://github.com/x"), "http");
    expect(!isSafeDownloadUrl("https://api.github.com/x"), "api host is not a download host");
    expect(!isSafeDownloadUrl("https://github.com.evil.com/x"), "suffix lookalike");
    expect(!isSafeDownloadUrl("https://evilgithub.com/x"), "prefix lookalike");
    expect(!isSafeDownloadUrl("https://evil.com/github.com/x"), "host in the path");
    expect(!isSafeDownloadUrl("https://evil.com?github.com"), "host in the query");
    expect(!isSafeDownloadUrl("https://evil.com#@github.com/x"), "host in the fragment");
    expect(!isSafeDownloadUrl("https://github.com@evil.com/x"), "userinfo before evil host");
    expect(!isSafeDownloadUrl("https://user:pw@github.com/x"), "userinfo before real host");
    expect(!isSafeDownloadUrl("https://github.com:8443/x"), "non-default port");
    expect(!isSafeDownloadUrl("https://github.com:/x"), "empty port");
    expect(!isSafeDownloadUrl("https://github.com./x"), "trailing dot host");
    expect(!isSafeDownloadUrl("https://github.com\\@evil.com/x"), "backslash");
    expect(!isSafeDownloadUrl("https://github.com /x"), "space");
    expect(!isSafeDownloadUrl("https://github.com\n.evil.com/x"), "newline");
    expect(!isSafeDownloadUrl("ftp://github.com/x"), "other scheme");
    expect(!isSafeDownloadUrl("//github.com/x"), "scheme-relative");
    expect(!isSafeDownloadUrl("https://"), "no host");
    expect(!isSafeDownloadUrl(""), "empty");

    // A signed query string must reach the wire untouched ('+' would otherwise become a space).
    {
        const juce::String signed_url = "https://release-assets.githubusercontent.com/a/b?sig=a+b%2Bc%3D&se=2026";
        expect(makeRawUrl(signed_url).toString(true) == signed_url, "query string is preserved verbatim");
        expect(makeRawUrl("https://github.com/x/y.exe").toString(true) == "https://github.com/x/y.exe", "plain URL");
    }

    // The checksum file.
    {
        const auto good = kHash + "  " + kName + "\n";
        expect(parseChecksumFile(good, kName) == kHash, "sha256sum text format");
        expect(parseChecksumFile(kHash + " *" + kName + "\n", kName) == kHash, "binary marker");
        expect(parseChecksumFile(kHash + "  " + kName, kName) == kHash, "no trailing newline");
        expect(parseChecksumFile(kHash + "  " + kName + "\r\n", kName) == kHash, "CRLF line ending");
        expect(parseChecksumFile(kHash.toUpperCase() + "  " + kName + "\n", kName) == kHash, "upper case hex is lowered");
        expect(parseChecksumFile(good, "QuarrySetup-1.2.4.exe").isEmpty(), "wrong file name");
        expect(parseChecksumFile(kHash + " " + kName + "\n", kName).isEmpty(), "single space");
        expect(parseChecksumFile(kHash + "   " + kName + "\n", kName).isEmpty(), "three spaces");
        expect(parseChecksumFile(kHash.dropLastCharacters(1) + "  " + kName + "\n", kName).isEmpty(), "63 hex digits");
        expect(parseChecksumFile(kHash + "0  " + kName + "\n", kName).isEmpty(), "65 hex digits");
        expect(parseChecksumFile("g" + kHash.substring(1) + "  " + kName + "\n", kName).isEmpty(), "non-hex digit");
        expect(parseChecksumFile(good + "\n", kName).isEmpty(), "extra blank line");
        expect(parseChecksumFile(good + good, kName).isEmpty(), "two lines");
        expect(parseChecksumFile(" " + good, kName).isEmpty(), "leading space");
        expect(parseChecksumFile(good + " ", kName).isEmpty(), "trailing space");
        expect(parseChecksumFile(kHash + "  " + kName + " extra\n", kName).isEmpty(), "text after the name");
        expect(parseChecksumFile("", kName).isEmpty(), "empty file");
        expect(parseChecksumFile(kHash + "\n", kName).isEmpty(), "hash only");
    }

    // Notes sanitising.
    {
        expect(sanitizeNotes("a\tb\nc") == "a\tb\nc", "tab and newline kept");
        expect(sanitizeNotes("a\r\nb") == "a\nb", "CRLF becomes LF");
        expect(sanitizeNotes(juce::String("a") + juce::String::charToString(0x1b) + "[31mb") == "a[31mb", "escape removed");
        expect(sanitizeNotes(juce::String::charToString(0) + "x" + juce::String::charToString(0x7f)) == "x", "NUL and DEL removed");
        expect(sanitizeNotes("x" + juce::String::charToString(0x85) + "y") == "xy", "C1 control removed");
        expect(sanitizeNotes("a" + juce::String::charToString(0x202e) + "b") == "ab", "bidi override removed");
        expect(sanitizeNotes(juce::String::repeatedString("x", 5000)).length() == 4000, "capped at 4000");
        expect(sanitizeNotes(juce::String::repeatedString("x", 50), 10).length() == 10, "custom cap");
        expect(sanitizeNotes(juce::String::repeatedString("\r", 5000) + "ok") == "ok", "stripped characters do not use the cap");
        expect(sanitizeNotes(juce::String::fromUTF8("Fixes: caf\xc3\xa9")).length() == 11, "ordinary unicode kept");

        expect(shortenNotes("## What's new\n- Faster", 40) == "What's new", "markdown marker dropped");
        expect(shortenNotes("\n\n  * First fix\nSecond", 40) == "First fix", "first line with words");
        expect(shortenNotes(juce::String::repeatedString("w", 100), 20) == juce::String::repeatedString("w", 17) + "...", "ellipsis");
        expect(shortenNotes("---\n\n", 20).isEmpty(), "no words");
        expect(shortenNotes("", 20).isEmpty(), "empty");
    }

    // The installer command line.
    expect(installerParameters(1234, false)
               == "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /COMPONENTS=\"mainapp\" /WAITPID=1234 /RELAUNCH",
           "parameters without the plugin");
    expect(installerParameters(77, true)
               == "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /COMPONENTS=\"mainapp,plugin\" /WAITPID=77 /RELAUNCH",
           "parameters with the plugin");

    // Names the release script and the installer must agree on.
    expect(installerFileName("1.2.3") == "QuarrySetup-1.2.3.exe", "installer name");
    expect(checksumFileName("1.2.3") == "QuarrySetup-1.2.3.exe.sha256", "checksum name");
    expect(expectedFileVersion("1.2.3") == "1.2.3.0", "file version");
    expect(hostBlockedMessage("Ableton Live")
               == "Ableton Live has the Quarry plugin loaded. Close it, then choose Install again.",
           "host blocked message");

    std::cout << (failures == 0 ? "  all passed" : "  FAILURES") << std::endl;
    return failures == 0;
}

#endif // QUARRY_UPDATE_POLICY_TEST_H
