//
// Thread-safe bounded ring of activity lines. Producers (the sidecar's stage events, its stderr
// pump, Quarry's own status lines) call add() from whatever thread they are on; the UI polls
// linesSince() on its own timer to pick up whatever landed since the last time it looked, rather
// than re-reading the whole log every tick.
//

#ifndef ActivityLog_h
#define ActivityLog_h

#include <algorithm>
#include <atomic>
#include <deque>
#include <vector>

#include <JuceHeader.h>

namespace quarry
{

/** One line in the log. Kind is what a UI would use to colour/icon the line; text is the line
 *  itself, already human-readable (a stage event's own "text" field, a raw stderr line, ...). */
struct ActivityLine
{
    enum class Kind { Quarry, Stage, Stderr, Error };

    Kind kind {};
    juce::int64 timeMs {}; // juce::Time::currentTimeMillis()
    juce::int64 seq {};    // monotonic, 1-based; see ActivityLog::revision()
    juce::String text;
};

/**
 * A bounded ring of ActivityLine. add() is the only thing that mutates it and is safe from any
 * thread; the log holds at most inCapacity lines, dropping the oldest once a new one would push
 * it over.
 *
 * seq is a monotonic counter that only ever goes up, even across a clear() or past whatever has
 * been evicted for capacity -- it counts lines this log has ever produced, not lines it currently
 * holds. That is what makes revision()/linesSince() work as a cheap "what's new" poll: a caller
 * remembers the highest seq it has already seen and asks for anything past it, without needing to
 * know or care whether earlier lines are still in the ring.
 */
class ActivityLog
{
public:
    explicit ActivityLog(int inCapacity = 4000) : mCapacity(inCapacity) {}

    /** The most lines this log will ever retain at once. What a UI mirroring the log into its
     *  own widget (a JUCE TextEditor, say) should trim its own copy down to, rather than
     *  guessing at or duplicating the number. */
    int capacity() const noexcept { return mCapacity; }

    /** Appends one line, evicting the oldest line(s) if the log is now over capacity. Callable
     *  from any thread. */
    void add(ActivityLine::Kind inKind, const juce::String& inText)
    {
        const juce::ScopedLock lock(mLock);

        ActivityLine line;
        line.kind = inKind;
        line.timeMs = juce::Time::currentTimeMillis();
        line.seq = ++mNextSeq;
        line.text = inText;

        mLines.push_back(std::move(line));

        while (static_cast<int>(mLines.size()) > mCapacity) {
            mLines.pop_front();
        }

        // Written last, after the line is fully in mLines, and read without the lock: a reader
        // that observes the new revision() and then takes the lock in linesSince() is guaranteed
        // to find the line that produced it already there.
        mRevision.store(line.seq, std::memory_order_release);
    }

    /** The seq of the newest line this log has ever produced (not necessarily one it still
     *  holds), or 0 if add() has never been called. A single atomic load, no lock, so a UI timer
     *  can check "has anything changed" every tick without contending with a producer thread. */
    juce::int64 revision() const noexcept
    {
        return mRevision.load(std::memory_order_acquire);
    }

    /** Every retained line with seq > inSeq, oldest first. Pass the seq of the last line already
     *  seen (0 to get everything currently retained) to get only what is new. A line at or below
     *  inSeq that has since been evicted for capacity is simply not in the result -- there is no
     *  way to ask for something the ring no longer holds. */
    std::vector<ActivityLine> linesSince(juce::int64 inSeq) const
    {
        const juce::ScopedLock lock(mLock);

        // mLines is in seq order -- add() only ever appends a larger seq than the one before it
        // -- so "everything past inSeq" is always a contiguous run at the back, not something
        // that needs walking the whole deque to find. A drawer polling at 10 Hz against a log at
        // its 4000-line capacity was doing 40,000 seq comparisons and a 4000-element reserve()
        // a second to hand back the one or two lines that actually arrived; a binary search for
        // where the run starts, then a range-construct sized to exactly what is returned, makes
        // that cost proportional to the new lines instead of to the whole log.
        const auto begin = std::upper_bound(
            mLines.begin(), mLines.end(), inSeq,
            [](juce::int64 inTargetSeq, const ActivityLine& inLine) { return inTargetSeq < inLine.seq; });

        return std::vector<ActivityLine>(begin, mLines.end());
    }

    /** Every line currently retained, oldest first. Equivalent to linesSince(0) except when the
     *  log has evicted lines for capacity, in which case that's exactly the difference: this
     *  still only returns what is still held. */
    std::vector<ActivityLine> snapshot() const
    {
        const juce::ScopedLock lock(mLock);
        return std::vector<ActivityLine>(mLines.begin(), mLines.end());
    }

    /** Empties the log. Leaves revision() exactly where it was: it counts lines ever produced,
     *  and clear() does not produce one, so a caller mid-poll does not see revision() go
     *  backwards just because the log was cleared out from under it. */
    void clear()
    {
        const juce::ScopedLock lock(mLock);
        mLines.clear();
    }

private:
    const int mCapacity;

    mutable juce::CriticalSection mLock;
    std::deque<ActivityLine> mLines;
    juce::int64 mNextSeq = 0;

    std::atomic<juce::int64> mRevision { 0 };
};

} // namespace quarry

#endif // ActivityLog_h
