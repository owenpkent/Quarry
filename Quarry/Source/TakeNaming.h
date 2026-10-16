//
// Naming shared by everything that writes a take to the folder you picked.
//

#ifndef TakeNaming_h
#define TakeNaming_h

#include <JuceHeader.h>

#include "NnId.h"

namespace TakeNaming
{
constexpr int kMaxStemAttempts = 999;

/** A folder stored in the project is only honoured when it is an absolute path, since a project can
    come from anywhere and a relative one would resolve against whatever the host's working
    directory happens to be. */
inline bool isUsableStoredFolder(const String& inStored)
{
    return inStored.isNotEmpty() && File::isAbsolutePath(inStored);
}

/** The folder takes land in, resolved the one way for every caller that writes one.

    The sample bar picks it and the drag tile follows, so a dragged transcription ends up
    beside the take it came from. Both read SampleFolderId rather than holding a folder
    each: one folder chosen once is the whole point of the setting, and a default spelled
    in two places is a default that eventually disagrees with itself.
*/
inline File folder(const ValueTree& inState)
{
    const String stored = inState.getProperty(NnId::SampleFolderId, String());

    if (isUsableStoredFolder(stored))
        return File(stored);

    return File::getSpecialLocation(File::userMusicDirectory).getChildFile("Quarry Samples");
}

/** The folder the Sample page browses: raw captures, and by default downloads too. */
inline File captureFolder(const ValueTree& inState)
{
    const String stored = inState.getProperty(NnId::CaptureFolderId, String());

    if (isUsableStoredFolder(stored))
        return File(stored);

    return File::getSpecialLocation(File::userMusicDirectory).getChildFile("Quarry Captures");
}

/** Where a link download is saved: the folder chosen for downloads, or the capture folder until
    one is chosen, so a download lands where the Sample page can list it. */
inline File downloadFolder(const ValueTree& inState)
{
    const String stored = inState.getProperty(NnId::DownloadFolderId, String());

    return isUsableStoredFolder(stored) ? File(stored) : captureFolder(inState);
}

/** "audio", "video" or "both"; anything else stored reads as audio. */
inline String downloadMedia(const ValueTree& inState)
{
    const String stored = inState.getProperty(NnId::DownloadMediaId, String("audio"));

    return stored == "video" || stored == "both" ? stored : String("audio");
}

/** A stem free for every extension the caller writes, so a pair cannot drift apart.
    Empty when every candidate is taken, which the caller has to report rather than
    write over somebody's take.
*/
inline String freeStem(const File& inFolder, const String& inBase, const StringArray& inExtensions)
{
    for (int attempt = 1; attempt <= kMaxStemAttempts; attempt++) {
        const auto stem = attempt == 1 ? inBase : inBase + " (" + String(attempt) + ")";
        bool taken = false;

        for (const auto& extension: inExtensions)
            taken = taken || inFolder.getChildFile(stem + extension).existsAsFile();

        if (!taken)
            return stem;
    }

    return {};
}
} // namespace TakeNaming

#endif // TakeNaming_h
