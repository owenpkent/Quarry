//
// Decides whether a piece of text is a link the sidecar's download request can be given.
//
// Three surfaces feed one entry point (the activity drawer's Enter, Ctrl+V on the main view, and a
// link dragged onto the waveform), and only the first is text somebody typed on purpose. A browser
// drag can carry the URL and the page title on separate lines, the clipboard can carry a trailing
// newline, and a paste can just as easily be a file path or a sentence. Pure and header-only so
// Tests/url_drop_test.h can pin every one of those cases without a GUI.
//

#ifndef UrlDrop_h
#define UrlDrop_h

#include <JuceHeader.h>

namespace quarry
{

/**
 * The URL in `inText`, or an empty string if there is not one worth downloading. Whitespace around
 * the text is dropped and only the first line is considered. The scheme must be http:// or
 * https:// (any case) with something after it, and the URL must not contain whitespace.
 */
inline juce::String downloadableUrl(const juce::String& inText)
{
    const auto first_line = inText.trim().upToFirstOccurrenceOf("\n", false, false).trim();

    if (!first_line.startsWithIgnoreCase("http://") && !first_line.startsWithIgnoreCase("https://"))
        return {};

    if (first_line.fromFirstOccurrenceOf("://", false, false).isEmpty())
        return {};

    for (auto c = first_line.getCharPointer(); !c.isEmpty(); ++c)
        if (juce::CharacterFunctions::isWhitespace(*c))
            return {};

    return first_line;
}

} // namespace quarry

#endif // UrlDrop_h
