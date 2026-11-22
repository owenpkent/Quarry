#pragma once

#include <juce_core/juce_core.h>

//
// Dragging a take out of the library and into another application.
//
// A row in the library is a file on disk, and a DAW takes a file dropped on a track. The list
// starts an ordinary drag that carries the take's path; once the pointer has left Quarry's
// window the page is asked what to hand the system, and the answer is that one file. It is
// offered as a copy, never a move: a take moved out of the library leaves a sidecar describing
// nothing.
//
// Kept apart from the view so the decisions it makes (which rows are takes, and whether a
// description still names a file worth dropping) can be tested without a window. juce_core only,
// like the library it sits beside.
//

namespace quarry::sampler
{

/** The library list is the way back up, then the folders, then the takes. Returns the index into
    the takes for a list row, or -1 for a row that is somewhere to go and not something to drag. */
inline int takeIndexForRow(int row, int upRows, int folderRows, int takeRows) noexcept
{
    const auto index = row - upRows - folderRows;

    return (row >= 0 && index >= 0 && index < takeRows) ? index : -1;
}

/** What a drag of this take carries: its path. Void when there is nothing on disk to drop, which
    is how the list is told not to start a drag at all. */
inline juce::var takeDragDescription(const juce::File& audioFile)
{
    if (! audioFile.existsAsFile())
        return {};

    return audioFile.getFullPathName();
}

/** The files to hand the system when a drag carrying `description` leaves the window. False,
    with nothing added, unless it names a file that is still there: a take deleted between the
    press and the edge of the window drops nothing. */
inline bool filesForTakeDrag(const juce::var& description, juce::StringArray& files)
{
    if (! description.isString())
        return false;

    const auto path = description.toString();

    if (! juce::File::isAbsolutePath(path))
        return false;

    const juce::File file(path);

    if (! file.existsAsFile())
        return false;

    files.add(file.getFullPathName());
    return true;
}

} // namespace quarry::sampler
