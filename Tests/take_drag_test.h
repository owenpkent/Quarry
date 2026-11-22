//
// A take dragged out of the library has to be the take that was pressed, and a file that is there.
//
// The library list is three kinds of row in one list (the way back up, folders, takes) and only
// the last is something to drag. What leaves the window is a path handed to another application,
// so a description that is not a path, or names a file that has gone, must drop nothing.
//

#ifndef QUARRY_TAKE_DRAG_TEST_H
#define QUARRY_TAKE_DRAG_TEST_H

#include <iostream>
#include <string>

#include "Sampler/TakeDrag.h"

namespace take_drag_test_utils
{
static int failures = 0;

static void expect(bool condition, const std::string& what)
{
    if (! condition)
    {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}
} // namespace take_drag_test_utils

inline bool take_drag_test()
{
    using namespace take_drag_test_utils;
    using namespace quarry::sampler;

    // One row up, two folders, three takes.
    expect(takeIndexForRow(0, 1, 2, 3) == -1, "the way back up is not a take");
    expect(takeIndexForRow(1, 1, 2, 3) == -1, "a folder is not a take");
    expect(takeIndexForRow(2, 1, 2, 3) == -1, "the last folder is not a take");
    expect(takeIndexForRow(3, 1, 2, 3) == 0, "the first row after the folders is the first take");
    expect(takeIndexForRow(5, 1, 2, 3) == 2, "the last row is the last take");
    expect(takeIndexForRow(6, 1, 2, 3) == -1, "a row past the end is nothing");
    expect(takeIndexForRow(-1, 0, 0, 3) == -1, "a negative row is nothing");
    expect(takeIndexForRow(0, 0, 0, 3) == 0, "at the root with no folders the first row is a take");
    expect(takeIndexForRow(0, 0, 0, 0) == -1, "an empty library has nothing to drag");

    const auto take = juce::File::createTempFile("_quarry_take_drag.wav");
    expect(take.replaceWithText("not really audio"), "the test take could be written");

    const auto description = takeDragDescription(take);
    expect(description.isString(), "a take on disk has a drag description");
    expect(description.toString() == take.getFullPathName(), "the description is the take's path");

    juce::StringArray files;
    expect(filesForTakeDrag(description, files), "a take on disk can be dropped");
    expect(files.size() == 1 && files[0] == take.getFullPathName(), "exactly that file is dropped");

    // Deleted between the press and the edge of the window.
    expect(take.deleteFile(), "the test take could be deleted");

    juce::StringArray none;
    expect(! filesForTakeDrag(description, none), "a take that has gone drops nothing");
    expect(none.isEmpty(), "and adds nothing to the list");
    expect(takeDragDescription(take).isVoid(), "a missing take starts no drag");

    expect(! filesForTakeDrag(juce::var(), none), "an empty description drops nothing");
    expect(! filesForTakeDrag(juce::var(3), none), "a number drops nothing");
    expect(! filesForTakeDrag(juce::var("take.wav"), none), "a relative path drops nothing");
    expect(none.isEmpty(), "none of those added a file");

    std::cout << (failures == 0 ? "  all passed" : "  FAILURES") << std::endl;
    return failures == 0;
}

#endif // QUARRY_TAKE_DRAG_TEST_H
