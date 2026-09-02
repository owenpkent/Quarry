//
// ActivityLog: add(), linesSince(), and capacity eviction. Single-threaded here -- the class's
// own thread-safety is a locking/atomics story a unit test cannot usefully exercise beyond
// "does it compile and behave correctly used from one thread"; that is what this checks.
//

#ifndef ACTIVITY_LOG_TEST_H
#define ACTIVITY_LOG_TEST_H

#include <iostream>

#include <JuceHeader.h>

#include "ActivityLog.h"

namespace activity_log_test_detail
{
inline bool expect(bool inCondition, const char* inWhat, bool& ioOk)
{
    if (!inCondition) {
        std::cout << "  FAIL: " << inWhat << std::endl;
        ioOk = false;
    }

    return inCondition;
}

inline bool testAddAndLinesSince()
{
    bool ok = true;

    quarry::ActivityLog log(100);

    expect(log.revision() == 0, "a fresh log has revision 0", ok);
    expect(log.snapshot().empty(), "a fresh log has no lines", ok);
    expect(log.linesSince(0).empty(), "a fresh log has nothing since seq 0", ok);

    log.add(quarry::ActivityLine::Kind::Quarry, "first");
    log.add(quarry::ActivityLine::Kind::Stage, "second");
    log.add(quarry::ActivityLine::Kind::Stderr, "third");

    expect(log.revision() == 3, "revision tracks the newest line's seq", ok);

    const auto all = log.linesSince(0);

    if (expect(all.size() == 3, "linesSince(0) returns everything retained", ok)) {
        expect(all[0].text == "first" && all[0].seq == 1, "oldest line first, with seq 1", ok);
        expect(all[1].text == "second" && all[1].seq == 2, "middle line next, with seq 2", ok);
        expect(all[2].text == "third" && all[2].seq == 3, "newest line last, with seq 3", ok);
        expect(all[0].kind == quarry::ActivityLine::Kind::Quarry, "kind survives the round trip", ok);
        expect(all[0].timeMs > 0, "timeMs is stamped", ok);
    }

    const auto since_first = log.linesSince(1);

    if (expect(since_first.size() == 2, "linesSince(1) skips the already-seen line", ok)) {
        expect(since_first[0].text == "second", "first new line is \"second\"", ok);
        expect(since_first[1].text == "third", "second new line is \"third\"", ok);
    }

    expect(log.linesSince(3).empty(), "linesSince(newest seq) returns nothing new", ok);
    expect(log.linesSince(1000).empty(), "linesSince(a seq past the newest) returns nothing", ok);

    return ok;
}

inline bool testCapacityEviction()
{
    bool ok = true;

    quarry::ActivityLog log(3);

    for (int i = 1; i <= 5; ++i) {
        log.add(quarry::ActivityLine::Kind::Quarry, juce::String(i));
    }

    const auto retained = log.snapshot();

    if (expect(retained.size() == 3, "the log never grows past its capacity", ok)) {
        expect(retained[0].text == "3", "the two oldest lines were evicted first", ok);
        expect(retained[1].text == "4", "eviction keeps the survivors in order", ok);
        expect(retained[2].text == "5", "the newest line survives", ok);
    }

    expect(log.revision() == 5, "revision still counts every line ever added, not just retained ones", ok);

    // seq 2 was evicted, but it is still a valid "have I seen this" marker: everything with a
    // higher seq that is still retained comes back, nothing claims to have something it does not.
    const auto since_evicted = log.linesSince(2);

    if (expect(since_evicted.size() == 3, "linesSince() past an evicted seq returns what is still retained", ok)) {
        expect(since_evicted[0].text == "3", "starts from the oldest still-retained line", ok);
    }

    return ok;
}

inline bool testClear()
{
    bool ok = true;

    quarry::ActivityLog log(10);
    log.add(quarry::ActivityLine::Kind::Quarry, "one");
    log.add(quarry::ActivityLine::Kind::Quarry, "two");

    const auto revision_before_clear = log.revision();

    log.clear();

    expect(log.snapshot().empty(), "clear() empties the log", ok);
    expect(log.revision() == revision_before_clear,
          "clear() does not move revision() backwards -- it only ever counts lines produced", ok);

    log.add(quarry::ActivityLine::Kind::Quarry, "three");

    expect(log.revision() == revision_before_clear + 1, "a line added after clear() still gets the next seq", ok);
    expect(log.snapshot().size() == 1, "the log holds only what was added since the clear", ok);

    return ok;
}
} // namespace activity_log_test_detail

inline bool activity_log_test()
{
    bool ok = true;

    ok &= activity_log_test_detail::testAddAndLinesSince();
    ok &= activity_log_test_detail::testCapacityEviction();
    ok &= activity_log_test_detail::testClear();

    return ok;
}

#endif // ACTIVITY_LOG_TEST_H
