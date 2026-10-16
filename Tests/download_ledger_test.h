//
// Which completion belongs to which download request, and when a partial-download warning stays
// on the Sample page's status line. Both are pure (DownloadLedger.h), so no sidecar or window.
//

#ifndef QUARRY_DOWNLOAD_LEDGER_TEST_H
#define QUARRY_DOWNLOAD_LEDGER_TEST_H

#include <iostream>
#include <string>

#include "DownloadLedger.h"

namespace download_ledger_test_utils
{
static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cout << "  FAILED: " << what << std::endl;
        ++failures;
    }
}

static quarry::DownloadResult resultFor(std::uint32_t id, const juce::String& message)
{
    quarry::DownloadResult result;
    result.requestId = id;
    result.ok = true;
    result.message = message;
    return result;
}
} // namespace download_ledger_test_utils

inline bool download_ledger_test()
{
    using namespace download_ledger_test_utils;
    failures = 0;

    // The drawer's slow download A is queued first, the Sample page's B second. A finishing must
    // not look like B finishing, and B's result must still be found after A's landed.
    {
        quarry::DownloadLedger ledger;
        const auto a = ledger.allocateId();
        const auto b = ledger.allocateId();
        quarry::DownloadResult out;

        check(a != 0 && b != 0 && a != b, "ids are distinct and never 0");
        check(!ledger.find(b, out), "B is not found before anything finished");

        ledger.publish(resultFor(a, "A"));
        check(!ledger.find(b, out), "A finishing is not B finishing");
        check(ledger.find(a, out) && out.message == "A", "A is found under its own id");

        ledger.publish(resultFor(b, "B"));
        check(ledger.find(b, out) && out.message == "B", "B is found under its own id");
        check(ledger.find(a, out) && out.message == "A", "A is still kept after B");
        check(ledger.seq() == 2, "seq counts publications");
        check(!ledger.find(0, out), "id 0 never matches");
    }

    // A burst of unrelated downloads between B finishing and the page polling must not lose B.
    {
        quarry::DownloadLedger ledger;
        const auto b = ledger.allocateId();
        quarry::DownloadResult out;

        ledger.publish(resultFor(b, "B"));

        for (int i = 0; i < 5; ++i)
            ledger.publish(resultFor(ledger.allocateId(), "other"));

        check(ledger.find(b, out) && out.message == "B", "B survives later downloads");
    }

    // Old entries fall off the back rather than growing for the life of the plugin.
    {
        quarry::DownloadLedger ledger;
        const auto first = ledger.allocateId();
        quarry::DownloadResult out;

        ledger.publish(resultFor(first, "first"));

        for (size_t i = 0; i < quarry::DownloadLedger::kKept; ++i)
            ledger.publish(resultFor(ledger.allocateId(), "later"));

        check(!ledger.find(first, out), "the oldest result is dropped past the cap");
    }

    // The partial-playlist warning survives the automatic selection, and nothing else.
    {
        quarry::DownloadNote note;
        const juce::String warning = "Downloaded 1 file(s), 1 failed.";

        check(note.resolve("Pick an application, then record.") == "Pick an application, then record.",
              "no note: the status shows");

        note.set(warning);
        check(note.resolve("Pick an application, then record.") == warning,
              "before the scan the note is shown");

        note.anchor("Clip");
        check(note.resolve("Clip") == warning, "the note outlasts the automatic selection");
        check(note.resolve("Clip") == warning, "and keeps doing so");
        check(note.isActive(), "still active");

        check(note.resolve("Recording 1.0 s") == "Recording 1.0 s", "a later message replaces the note");
        check(!note.isActive(), "and ends it");
        check(note.resolve("Clip") == "Clip", "the old title does not bring it back");

        note.set(warning);
        note.anchor("Clip");
        note.clear();
        check(note.resolve("Clip") == "Clip", "clearing (next download, click) ends it");
    }

    std::cout << (failures == 0 ? "  all passed" : "  FAILURES") << std::endl;
    return failures == 0;
}

#endif // QUARRY_DOWNLOAD_LEDGER_TEST_H
