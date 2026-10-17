//
// The rules the updater applies, with nothing platform-specific in them: which release is newer,
// which assets belong to it, which URLs may be fetched, what a checksum file may contain, and
// what is allowed to reach the screen. Pure functions over strings so every one of them can be
// tested without a network, and so the same code compiles on every platform even though only
// the Windows Standalone installs anything. docs/AUTO_UPDATE.md is the written form.
//

#ifndef UPDATEPOLICY_H
#define UPDATEPOLICY_H

#include <JuceHeader.h>

#include <optional>

namespace quarry::update
{
struct Version
{
    int major {0};
    int minor {0};
    int patch {0};

    bool operator==(const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
    bool operator<(const Version& o) const
    {
        if (major != o.major)
            return major < o.major;
        if (minor != o.minor)
            return minor < o.minor;
        return patch < o.patch;
    }
};

/** Strictly "X.Y.Z": three plain decimal numbers, no leading zeros, no sign, no suffix. */
std::optional<Version> parseVersion(const juce::String& inText);

/** Strictly "vX.Y.Z", the only shape of release tag the updater acts on. */
std::optional<Version> parseTag(const juce::String& inTag);

/** True only when the tag is well formed, the running version is well formed, and the tag is
 *  strictly greater. Equal, older and malformed all answer false. */
bool isNewer(const juce::String& inCandidateTag, const juce::String& inCurrentVersion);

juce::String versionToString(const Version& inVersion);

/** "QuarrySetup-X.Y.Z.exe" and the same with ".sha256" appended. */
juce::String installerFileName(const juce::String& inVersion);
juce::String checksumFileName(const juce::String& inVersion);

/** The FileVersion resource the installer for X.Y.Z must carry: "X.Y.Z.0". */
juce::String expectedFileVersion(const juce::String& inVersion);

/** https only, no userinfo, no port other than the default, and a host that is exactly one of
 *  the GitHub release hosts. api.github.com is deliberately absent: it is the API call's host
 *  and never somewhere a download may come from. */
bool isSafeDownloadUrl(const juce::String& inUrl);

/** The lowercase host of an https URL, or empty if it is not one. Used to resolve a relative
 *  redirect against a URL that already passed isSafeDownloadUrl. */
juce::String hostOf(const juce::String& inUrl);

/** A URL whose path and query reach the wire exactly as written. juce::URL(String) splits the
 *  query into parameters, turns '+' into a space and re-escapes the rest, which would corrupt
 *  the signed query string GitHub's asset redirect carries. Empty origin parts are not handled:
 *  pass only URLs that passed isSafeDownloadUrl. */
juce::URL makeRawUrl(const juce::String& inUrl);

struct ReleaseInfo
{
    juce::String tag;     // "vX.Y.Z"
    juce::String version; // "X.Y.Z"
    juce::String notes;   // sanitized
    juce::String installerUrl;
    juce::String checksumUrl;

    /** Both assets were present, named exactly, and on an allowed host. Without them the
     *  release can be announced but not installed. */
    bool hasInstallableAssets() const { return installerUrl.isNotEmpty() && checksumUrl.isNotEmpty(); }
};

/** Reads a /releases/latest response. Nothing comes back for a draft, a prerelease or a tag that
 *  is not strict vX.Y.Z. Assets are matched by exact name, never by suffix. */
std::optional<ReleaseInfo> parseRelease(const juce::var& inJson);

/** The lowercase hex digest from a sha256sum-format file for the given file name, or an empty
 *  string if the text is anything else: wrong name, wrong length, extra lines, extra bytes. */
juce::String parseChecksumFile(const juce::String& inText, const juce::String& inExpectedFileName);

/** Control characters other than newline and tab removed (carriage returns included, so CRLF
 *  becomes LF), along with the invisible bidirectional overrides that can reorder displayed
 *  text, then cut to inMaxChars. */
juce::String sanitizeNotes(const juce::String& inRaw, int inMaxChars = 4000);

/** One readable line for the banner: the first line that has words in it, markdown markers
 *  dropped, shortened with an ellipsis. */
juce::String shortenNotes(const juce::String& inSanitized, int inMaxChars);

/** The installer's command line. inPluginInstalled adds the VST3 component to the update. */
juce::String installerParameters(int inProcessId, bool inPluginInstalled);

/** The banner text for a host that has the plugin open, built from the name the OS gave. */
juce::String hostBlockedMessage(const juce::String& inHostName);
} // namespace quarry::update

#endif // UPDATEPOLICY_H
