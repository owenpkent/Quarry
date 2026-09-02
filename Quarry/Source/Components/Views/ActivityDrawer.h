//
// The dev-facing overlay that shows what Quarry and the sidecar have been doing: a terminal-
// style log with a one-line prompt underneath it for pasting a URL in.
//

#ifndef ActivityDrawer_h
#define ActivityDrawer_h

#include "JuceHeader.h"

#include "ActivityLog.h"
#include "UIDefines.h"

/**
    A drawer rather than a fourth left-column section, because there is no room for one --
    ActivityDrawerLayout says why. Hidden until something opens it, drawn last so it sits over
    the column and the footer, and closed by its own button or by whoever owns it.

    Knows nothing about TranscriptionManager or the processor: it is handed the log to read and
    two callbacks, one for the status line and one for what Return in the prompt means, so it
    can be built and reasoned about on its own.
*/
class ActivityDrawer
    : public Component
    , public Timer
{
public:
    ActivityDrawer(quarry::ActivityLog& inLog,
                   std::function<String()> inStatusText,
                   std::function<void(const String&)> inOnCommand);

    ~ActivityDrawer() override;

    /** Shows the drawer, rebuilds the log from a fresh snapshot, and hands the prompt keyboard
        focus. */
    void open();

    /** Hides the drawer and calls onClosed, so the owner can take focus back. */
    void close();

    /** Called after close(), whichever way it happened -- the button or the owner's own
        keypress. Not called by setVisible(false) alone: only by close(). */
    std::function<void()> onClosed;

    void paint(Graphics& g) override;

    void resized() override;

    void visibilityChanged() override;

    void timerCallback() override;

private:
    /** Appends inLines to the log view, advances mLastSeq past the last of them, and trims the
        view if that pushed it meaningfully over the log's own capacity. Keeps a live selection
        where it is rather than scrolling under it, unless a trim happened underneath it. */
    void _appendLines(const std::vector<quarry::ActivityLine>& inLines);

    /** Inserts inLines into mLogView, batching consecutive same-Kind lines into one setColour
        plus one insertTextAtCaret rather than paying both costs per line. Does not touch
        mLastSeq, mDisplayedLines, the caret, or the selection -- callers own all of that,
        because both _appendLines and the trim in _appendLines need this same insertion but with
        different bookkeeping around it. */
    void _insertLines(const std::vector<quarry::ActivityLine>& inLines);

    /** If mDisplayedLines has grown far enough past the log's capacity to be worth the cost,
        rebuilds mLogView from mLog.snapshot() (which is already capped there) and returns true.
        Otherwise does nothing and returns false. Called from _appendLines, not on every append:
        see the comment on mDisplayedLines. */
    bool _trimIfNeeded();

    /** Rebuilds the log view from scratch, for the moment the drawer opens: the log can have
        moved while it was hidden and a timer that only diffs would miss all of it. */
    void _rebuildFromSnapshot();

    Colour _colourFor(quarry::ActivityLine::Kind inKind) const;

    /** Return in the prompt: trim, do nothing on empty, "clear" wipes the log, anything else
        goes to mOnCommand. Handled here so the caller never sees "clear" as a command. */
    void _handleReturn();

    quarry::ActivityLog& mLog;
    std::function<String()> mStatusText;
    std::function<void(const String&)> mOnCommand;

    juce::int64 mLastSeq = 0;
    String mLastStatus;

    // How many lines mLogView currently holds. Kept as a running count, updated alongside every
    // insert and every trim, rather than asked of the editor on each append: JUCE has no O(1)
    // line count, only getTotalNumChars() plus a scan for newlines, and this is checked on every
    // single append just to decide whether a trim is due. See _trimIfNeeded.
    int mDisplayedLines = 0;

    std::unique_ptr<TextEditor> mLogView;
    std::unique_ptr<TextEditor> mPrompt;
    std::unique_ptr<DrawableButton> mCloseButton;

    Font mMonoPromptFont { FontOptions(Font::getDefaultMonospacedFontName(), 12.0f, Font::plain) };

    // Set in resized(), read in paint(): the header's text row and the glyph cell ahead of the
    // prompt, so the layout arithmetic lives in one place.
    Rectangle<int> mHeaderTextArea;
    Rectangle<int> mPromptGlyphArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivityDrawer)
};

#endif // ActivityDrawer_h
