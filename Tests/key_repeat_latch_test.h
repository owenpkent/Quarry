//
// A held key must be one click, not one per repeat.
//
// The space bar on the Sample page toggles a recording. Windows repeats a held key as a stream of
// key presses, and each one reached the toggle: hold the key and the take stopped on the first
// repeat, started again on the second, and so on. This drives the latch the view puts in front
// of the toggle with that stream: a press, repeats with no release, a release, a second press.
//

#ifndef QUARRY_KEY_REPEAT_LATCH_TEST_H
#define QUARRY_KEY_REPEAT_LATCH_TEST_H

#include <iostream>
#include <string>

#include "Components/KeyRepeatLatch.h"

namespace key_repeat_latch_test_utils
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

// What the view does with a key press: toggle only if the latch says this is the press itself.
struct Recorder
{
    quarry::KeyRepeatLatch latch;
    bool recording {false};
    int starts {0};
    int stops {0};

    void keyPress()
    {
        if (! latch.press())
            return;

        recording = ! recording;
        ++(recording ? starts : stops);
    }
};
} // namespace key_repeat_latch_test_utils

inline bool key_repeat_latch_test()
{
    using namespace key_repeat_latch_test_utils;

    Recorder recorder;

    // The press, then the key held through eight repeats.
    recorder.keyPress();
    for (int i = 0; i < 8; ++i)
        recorder.keyPress();

    expect(recorder.starts == 1, "a held key starts exactly one take");
    expect(recorder.stops == 0, "a held key does not stop the take it started");
    expect(recorder.recording, "still recording while the key is held");
    expect(recorder.latch.isHeld(), "the latch is held until release");

    // Release, then a second press, also held.
    recorder.latch.release();
    expect(! recorder.latch.isHeld(), "release clears the latch");

    recorder.keyPress();
    for (int i = 0; i < 8; ++i)
        recorder.keyPress();

    expect(recorder.stops == 1, "the second press stops exactly once");
    expect(recorder.starts == 1, "the second press does not start another take");
    expect(! recorder.recording, "not recording after the second press");

    // A release with nothing held (focus moving about) changes nothing.
    recorder.latch.release();
    recorder.latch.release();
    recorder.keyPress();
    expect(recorder.starts == 2, "a press after a stray release still counts once");

    std::cout << (failures == 0 ? "  all passed" : "  FAILURES") << std::endl;
    return failures == 0;
}

#endif // QUARRY_KEY_REPEAT_LATCH_TEST_H
