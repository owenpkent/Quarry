//
// Reading a folder of captures back.
//

#include "SampleLibrary.h"

#include <algorithm>

namespace quarry::sampler
{

namespace
{
juce::String textOf(const juce::var& object, const char* property)
{
    // A null in the sidecar parses to a void var, which is exactly what an absent field
    // should look like here: the schema writes null for everything not yet gathered.
    const auto value = object.getProperty(property, juce::var());
    return value.isVoid() ? juce::String() : value.toString();
}

double numberOf(const juce::var& object, const char* property, double fallback)
{
    const auto value = object.getProperty(property, juce::var());
    return value.isDouble() || value.isInt() || value.isInt64() ? (double) value : fallback;
}

} // namespace

/**
 * This string arrives from a file, and the whole point of the sidecar design is that
 * captures travel between machines, so it is not ours and must not be trusted. JUCE resolves
 * "../" inside getSiblingFile, and the result is later handed to moveToTrash: a crafted
 * sidecar in a downloaded sample pack could otherwise delete something else entirely.
 *
 * Checked twice on purpose. The character test rejects the obvious shapes, and comparing the
 * resolved parent catches whatever the first test did not think of.
 */
juce::File SampleLibrary::siblingNamed(const juce::File& sidecar, const juce::String& name)
{
    if (name.isEmpty() || name.containsAnyOf("/\\:") || name.contains(".."))
        return {};

    const auto resolved = sidecar.getSiblingFile(name);

    if (resolved.getParentDirectory() != sidecar.getParentDirectory())
        return {};

    return resolved;
}

LibraryEntry SampleLibrary::parse(const juce::File& sidecar)
{
    LibraryEntry entry;
    entry.sidecarFile = sidecar;
    entry.audioFile = sidecar.getSiblingFile(sidecar.getFileNameWithoutExtension() + ".wav");

    const auto parsed = juce::JSON::parse(sidecar.loadFileAsString());

    if (! parsed.isObject())
        return entry;

    const auto source = parsed.getProperty("source", juce::var());
    const auto audio = parsed.getProperty("audio", juce::var());

    if (source.isObject())
    {
        entry.appName = textOf(source, "processName");
        entry.windowTitle = textOf(source, "windowTitle");
        entry.url = textOf(source, "url");
        entry.isolatedToProcess = textOf(source, "isolation") != "endpoint";

        entry.imageFile = siblingNamed(sidecar, textOf(source, "screenshot"));
    }

    if (audio.isObject())
    {
        entry.durationSec = numberOf(audio, "durationSec", 0.0);
        entry.lufs = (float) numberOf(audio, "lufs", -144.0);
        entry.peakDb = (float) numberOf(audio, "peakDb", -144.0);
    }

    if (const auto tags = parsed.getProperty("tags", juce::var()); tags.isArray())
        for (const auto& tag : *tags.getArray())
            entry.tags.add(tag.toString());

    // The timestamp is the one field worth falling back for: without it the browser cannot
    // order itself, and the file's own modification time is close enough to be useful.
    const auto stamped = juce::Time::fromISO8601(textOf(parsed, "capturedAt"));
    entry.capturedAt = stamped.toMilliseconds() > 0 ? stamped : sidecar.getLastModificationTime();

    entry.searchText = (entry.displayName() + " " + entry.appName + " " + entry.windowTitle + " "
                        + entry.url + " " + entry.tags.joinIntoString(" "))
                           .toLowerCase();

    return entry;
}

std::vector<LibraryEntry> SampleLibrary::scan(const juce::File& root)
{
    std::vector<LibraryEntry> found;

    if (! root.isDirectory())
        return found;

    for (const auto& item : juce::RangedDirectoryIterator(root, true, "*.json", juce::File::findFiles))
    {
        auto entry = parse(item.getFile());

        // A sidecar whose audio has gone describes nothing, and a row that fails when
        // clicked is worse than a row that is not there.
        if (entry.audioFile.existsAsFile())
            found.push_back(std::move(entry));
    }

    std::sort(found.begin(), found.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        return a.capturedAt > b.capturedAt;
    });

    return found;
}

std::vector<LibraryEntry> SampleLibrary::filter(const std::vector<LibraryEntry>& entries,
                                                const juce::String& query)
{
    const auto trimmed = query.trim().toLowerCase();

    if (trimmed.isEmpty())
        return entries;

    // Every whitespace separated word has to appear somewhere, so "chrome drum" narrows
    // rather than widening, which is what typing a second word is for.
    juce::StringArray words;
    words.addTokens(trimmed, false);
    words.removeEmptyStrings();

    std::vector<LibraryEntry> kept;

    for (const auto& entry : entries)
    {
        auto matches = true;

        for (const auto& word : words)
        {
            if (! entry.searchText.contains(word))
            {
                matches = false;
                break;
            }
        }

        if (matches)
            kept.push_back(entry);
    }

    return kept;
}

bool SampleLibrary::remove(const LibraryEntry& entry)
{
    // To the recycle bin rather than deleted outright. This is someone's capture, and the
    // one thing worse than a browser without a delete button is one that is final.
    auto removed = entry.audioFile.moveToTrash();

    if (entry.sidecarFile.existsAsFile())
        removed = entry.sidecarFile.moveToTrash() && removed;

    if (entry.imageFile.existsAsFile())
        entry.imageFile.moveToTrash();

    return removed;
}

} // namespace quarry::sampler
