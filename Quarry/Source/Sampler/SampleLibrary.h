#pragma once

#include <juce_core/juce_core.h>

#include <vector>

//
// Everything captured so far, read back off disk.
//
// There is no database, and that is a decision rather than an omission. The sidecars are
// the record; this builds an index from them at startup and can be thrown away and rebuilt
// at any time. Delete a sample in Explorer and it is gone here too, with nothing left
// pointing at a file that is not there, which is the failure a database would have.
//
// juce_core only, so it stays testable and carries no Windows types: a library of captures
// is not a Windows idea even though making them is.
//

namespace quarry::sampler
{

/** One captured sample, as far as browsing is concerned. */
struct LibraryEntry
{
    juce::File audioFile;
    juce::File sidecarFile;
    juce::File imageFile;

    juce::Time capturedAt;
    juce::String appName;
    juce::String windowTitle;
    juce::String url;

    double durationSec = 0.0;
    float lufs = -144.0f;
    float peakDb = -144.0f;
    bool isolatedToProcess = true;

    juce::StringArray tags;

    /** Everything above, lower case, in one string. Built once so filtering a few thousand
        entries on every keystroke is a substring search rather than a rummage. */
    juce::String searchText;

    /** What the browser puts on the row. */
    juce::String displayName() const { return audioFile.getFileNameWithoutExtension(); }
};

/**
 * Reads a folder of captures.
 *
 * scan() walks the tree and parses every sidecar it finds. It is called off the message
 * thread by the browser, because a few thousand small files is a second of disk on a cold
 * cache and nothing about that should be felt in the UI.
 */
class SampleLibrary
{
public:
    /** Every sidecar under `root`, newest first. Missing audio is skipped: a sidecar whose
        wav has been deleted describes nothing, and offering it would only produce a row
        that fails when clicked. */
    static std::vector<LibraryEntry> scan(const juce::File& root);

    /** The subset matching `query`, which is matched against everything at once: the name,
        the application, the window title, the URL and the tags. One box, because the
        alternative is asking someone to know which field their memory of a sample lives
        in. An empty query keeps everything. */
    static std::vector<LibraryEntry> filter(const std::vector<LibraryEntry>& entries,
                                            const juce::String& query);

    /** Removes a capture and everything written beside it. */
    static bool remove(const LibraryEntry& entry);

    /**
     * A file named by a sidecar, resolved only if it really sits beside that sidecar, and an
     * invalid File otherwise.
     *
     * Public so it can be tested, because it is a security boundary rather than a
     * convenience. The name comes out of a file that may have arrived from another machine,
     * and what it names is later passed to moveToTrash.
     */
    static juce::File siblingNamed(const juce::File& sidecar, const juce::String& name);

private:
    static LibraryEntry parse(const juce::File& sidecar);
};

} // namespace quarry::sampler
