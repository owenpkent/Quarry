//
// How a download's outcome gets back to whoever asked for it, and how the Sample page keeps a
// partial-download warning on screen.
//
// Both are pure and header-only so Tests/download_ledger_test.h can pin them without a sidecar or
// a window.
//

#ifndef QUARRY_DOWNLOAD_LEDGER_H
#define QUARRY_DOWNLOAD_LEDGER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

#include <JuceHeader.h>

namespace quarry
{

/** How one download ended. */
struct DownloadResult {
    /** Order of publication; 0 until the first one finishes. */
    std::uint32_t seq = 0;

    /** The id requestDownload() returned for the request this answers. */
    std::uint32_t requestId = 0;
    bool ok = false;
    bool cancelled = false;

    /** The title on success, the error text otherwise. */
    juce::String message;

    /** Everything saved, wav first. */
    std::vector<juce::File> files;

    /** Playlist entries that failed; the rest are in files. */
    int failed = 0;
    juce::File outDir;
};

/**
 * Hands out request ids and keeps the most recent results by id.
 *
 * One "last result" slot was not enough: the activity drawer, a paste and the Sample page's link
 * box all feed the same single-thread queue, so a slow download from one of them finishes ahead
 * of a later request from another, and a page that took "whatever finished next" for its own
 * showed the wrong file and then ignored its real result. A requester asks for its own id.
 * Old entries fall off the back; a requester polls within a frame or two of the result landing,
 * so the window only has to cover a burst of downloads, not a session.
 */
class DownloadLedger
{
public:
    /** Any thread. Never 0, which callers use for "no request". */
    std::uint32_t allocateId() { return mNextId.fetch_add(1) + 1; }

    /** Any thread. The result's requestId says whose it is; seq is assigned here. */
    void publish(DownloadResult inResult)
    {
        const juce::ScopedLock lock(mLock);

        inResult.seq = mSeq.load() + 1;
        mRecent.push_back(std::move(inResult));

        while (mRecent.size() > kKept)
            mRecent.pop_front();

        // After the result is in place, so a poller that sees the new number finds what it counts.
        mSeq.store(mRecent.back().seq);
    }

    /** True and fills outResult if the request with this id has finished and is still kept. */
    bool find(std::uint32_t inRequestId, DownloadResult& outResult) const
    {
        if (inRequestId == 0)
            return false;

        const juce::ScopedLock lock(mLock);

        for (const auto& result : mRecent) {
            if (result.requestId == inRequestId) {
                outResult = result;
                return true;
            }
        }

        return false;
    }

    std::uint32_t seq() const { return mSeq.load(); }

    static constexpr size_t kKept = 32;

private:
    mutable juce::CriticalSection mLock;
    std::deque<DownloadResult> mRecent;
    std::atomic<std::uint32_t> mSeq { 0 };
    std::atomic<std::uint32_t> mNextId { 0 };
};

/**
 * A download outcome worth keeping on screen ("Downloaded 1 file(s), 1 failed.") beside the
 * Sample page's ordinary status line.
 *
 * The page selects the new capture on its own once the library rescan finds it, and selecting
 * writes the capture's title over the status line, which used to be the only place the warning
 * lived. The note outlasts that automatic write, and ends as soon as anything else speaks: the
 * user clears or restarts the link, clicks a row, or any later message replaces the status text
 * (it no longer equals the one the automatic selection wrote).
 */
class DownloadNote
{
public:
    /** A new outcome. Shown until the automatic selection has written its status, then held. */
    void set(const juce::String& inText)
    {
        mText = inText;
        mAnchored = false;
        mAnchor = {};
    }

    /** The status the automatic selection just wrote. The note stays while the status is this. */
    void anchor(const juce::String& inStatus)
    {
        if (mText.isEmpty())
            return;

        mAnchored = true;
        mAnchor = inStatus;
    }

    /** The user did something else: next download, clearing the link, choosing a row. */
    void clear()
    {
        mText = {};
        mAnchored = false;
        mAnchor = {};
    }

    bool isActive() const { return mText.isNotEmpty(); }

    /** What the status line should say given what the page's own status currently is. */
    juce::String resolve(const juce::String& inStatus)
    {
        if (mText.isEmpty())
            return inStatus;

        if (mAnchored && inStatus != mAnchor) {
            clear();
            return inStatus;
        }

        return mText;
    }

private:
    juce::String mText;
    juce::String mAnchor;
    bool mAnchored = false;
};

} // namespace quarry

#endif // QUARRY_DOWNLOAD_LEDGER_H
