#include "UpdatePolicy.h"

namespace quarry::update
{
namespace
{
// Large enough for any real version, small enough that three of them cannot overflow an int.
constexpr int kMaxVersionDigits = 6;

std::optional<int> parseComponent(const juce::String& inText)
{
    if (inText.isEmpty() || inText.length() > kMaxVersionDigits)
        return std::nullopt;

    for (auto c: inText) {
        if (c < '0' || c > '9')
            return std::nullopt;
    }

    // "01" and "1.02.3" are not what a release script writes, so they are not a version.
    if (inText.length() > 1 && inText[0] == '0')
        return std::nullopt;

    return inText.getIntValue();
}

bool isHex(juce::juce_wchar c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isBannedInNotes(juce::juce_wchar c)
{
    if (c == '\n' || c == '\t')
        return false;

    // C0 controls (carriage return among them), DEL and the C1 range.
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f))
        return true;

    // Zero-width and directional marks, and the embeddings and overrides that can make a
    // line of text read in a different order from the one it was written in.
    return (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)
           || c == 0xfeff;
}

// The authority of "https://authority/path", or false if the text is not that shape.
bool splitHttps(const juce::String& inUrl, juce::String& outAuthority)
{
    static const juce::String scheme("https://");
    if (!inUrl.startsWithIgnoreCase(scheme))
        return false;

    // Whitespace, control characters and backslashes are never in a URL we build, and
    // each is a way for two parsers to disagree about where the host ends.
    for (auto c: inUrl) {
        if (c <= 0x20 || c == 0x7f || c == '\\')
            return false;
    }

    const auto rest = inUrl.substring(scheme.length());
    const auto end = rest.indexOfAnyOf("/?#");
    outAuthority = end < 0 ? rest : rest.substring(0, end);
    return outAuthority.isNotEmpty();
}
} // namespace

std::optional<Version> parseVersion(const juce::String& inText)
{
    juce::StringArray parts;
    parts.addTokens(inText, ".", "");
    // addTokens drops empty tokens, so the dots are counted separately.
    if (inText.retainCharacters(".").length() != 2 || parts.size() != 3)
        return std::nullopt;

    const auto major = parseComponent(parts[0]);
    const auto minor = parseComponent(parts[1]);
    const auto patch = parseComponent(parts[2]);
    if (!major || !minor || !patch)
        return std::nullopt;

    return Version {*major, *minor, *patch};
}

std::optional<Version> parseTag(const juce::String& inTag)
{
    if (!inTag.startsWithChar('v'))
        return std::nullopt;
    return parseVersion(inTag.substring(1));
}

bool isNewer(const juce::String& inCandidateTag, const juce::String& inCurrentVersion)
{
    const auto candidate = parseTag(inCandidateTag);
    const auto current = parseVersion(inCurrentVersion);
    if (!candidate || !current)
        return false;
    return *current < *candidate;
}

juce::String versionToString(const Version& inVersion)
{
    return juce::String(inVersion.major) + "." + juce::String(inVersion.minor) + "." + juce::String(inVersion.patch);
}

juce::String installerFileName(const juce::String& inVersion)
{
    return "QuarrySetup-" + inVersion + ".exe";
}

juce::String checksumFileName(const juce::String& inVersion)
{
    return installerFileName(inVersion) + ".sha256";
}

juce::String expectedFileVersion(const juce::String& inVersion)
{
    return inVersion + ".0";
}

juce::String hostOf(const juce::String& inUrl)
{
    juce::String authority;
    if (!splitHttps(inUrl, authority))
        return {};
    return authority.toLowerCase();
}

bool isSafeDownloadUrl(const juce::String& inUrl)
{
    juce::String authority;
    if (!splitHttps(inUrl, authority))
        return false;

    // Userinfo: "https://github.com@evil.example/" has its real host after the @.
    if (authority.containsChar('@'))
        return false;

    auto host = authority;
    const auto colon = authority.indexOfChar(':');
    if (colon >= 0) {
        // Only the default port may be spelled out. Any other, or an empty one, is refused.
        if (authority.substring(colon + 1) != "443")
            return false;
        host = authority.substring(0, colon);
    }

    // Exact and case-insensitive. No suffix match: github.com.evil.example and
    // evilgithub.com both contain a name on the list.
    host = host.toLowerCase();
    return host == "github.com" || host == "objects.githubusercontent.com"
           || host == "release-assets.githubusercontent.com";
}

juce::URL makeRawUrl(const juce::String& inUrl)
{
    const auto scheme_length = juce::String("https://").length();
    const auto end = inUrl.indexOfAnyOf("/?#", scheme_length);
    if (end < 0)
        return juce::URL(inUrl);

    // The origin carries no query, so URL's parsing leaves it alone; the rest is appended as a
    // child path, which is concatenated verbatim.
    auto rest = inUrl.substring(end);
    if (rest.startsWithChar('/'))
        rest = rest.substring(1);
    return juce::URL(inUrl.substring(0, end)).getChildURL(rest);
}

std::optional<ReleaseInfo> parseRelease(const juce::var& inJson)
{
    if (!inJson.isObject())
        return std::nullopt;

    // The API flags a draft or prerelease with a boolean. "latest" should never return one,
    // but the stable-tags-only rule should not rest on that.
    if (static_cast<bool>(inJson.getProperty("draft", false))
        || static_cast<bool>(inJson.getProperty("prerelease", false)))
        return std::nullopt;

    const auto tag = inJson.getProperty("tag_name", juce::var()).toString().trim();
    const auto version = parseTag(tag);
    if (!version)
        return std::nullopt;

    ReleaseInfo info;
    info.tag = tag;
    info.version = versionToString(*version);
    info.notes = sanitizeNotes(inJson.getProperty("body", juce::var()).toString());

    const auto installer_name = installerFileName(info.version);
    const auto checksum_name = checksumFileName(info.version);

    if (const auto* assets = inJson.getProperty("assets", juce::var()).getArray()) {
        for (const auto& asset: *assets) {
            if (!asset.isObject())
                continue;

            const auto name = asset.getProperty("name", juce::var()).toString();
            const auto url = asset.getProperty("browser_download_url", juce::var()).toString();

            // A URL off the allowlist leaves the slot empty, so the release is announced but
            // cannot be installed; there is no second guess at a different asset.
            if (name == installer_name && isSafeDownloadUrl(url))
                info.installerUrl = url;
            else if (name == checksum_name && isSafeDownloadUrl(url))
                info.checksumUrl = url;
        }
    }

    return info;
}

juce::String parseChecksumFile(const juce::String& inText, const juce::String& inExpectedFileName)
{
    // "<64 hex>", two spaces (text mode) or a space and an asterisk (binary mode), the name,
    // and an optional line ending. Nothing before, nothing after.
    constexpr int kHexLength = 64;

    auto text = inText;
    if (text.endsWith("\r\n"))
        text = text.dropLastCharacters(2);
    else if (text.endsWith("\n"))
        text = text.dropLastCharacters(1);

    if (text.length() != kHexLength + 2 + inExpectedFileName.length())
        return {};

    for (int i = 0; i < kHexLength; ++i) {
        if (!isHex(text[i]))
            return {};
    }

    const auto separator = text.substring(kHexLength, kHexLength + 2);
    if (separator != "  " && separator != " *")
        return {};

    if (text.substring(kHexLength + 2) != inExpectedFileName)
        return {};

    return text.substring(0, kHexLength).toLowerCase();
}

juce::String sanitizeNotes(const juce::String& inRaw, int inMaxChars)
{
    juce::String out;
    int count = 0;

    for (auto c: inRaw) {
        if (isBannedInNotes(c))
            continue;
        if (count >= inMaxChars)
            break;
        out += c;
        ++count;
    }
    return out;
}

juce::String shortenNotes(const juce::String& inSanitized, int inMaxChars)
{
    for (const auto& raw_line: juce::StringArray::fromLines(inSanitized)) {
        // Headings, bullets and quotes are markup, not words.
        auto line = raw_line.trim().trimCharactersAtStart("#-*>+ \t").trim();
        if (line.isEmpty())
            continue;

        if (line.length() > inMaxChars)
            line = line.substring(0, juce::jmax(0, inMaxChars - 3)).trimEnd() + "...";
        return line;
    }
    return {};
}

juce::String installerParameters(int inProcessId, bool inPluginInstalled)
{
    return juce::String("/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /COMPONENTS=\"")
           + (inPluginInstalled ? "mainapp,plugin" : "mainapp") + "\" /WAITPID=" + juce::String(inProcessId)
           + " /RELAUNCH";
}

juce::String hostBlockedMessage(const juce::String& inHostName)
{
    const auto name = inHostName.isEmpty() ? juce::String("A DAW") : inHostName;
    return name + " has the Quarry plugin loaded. Close it, then choose Install again.";
}
} // namespace quarry::update
