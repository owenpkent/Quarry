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
    /** Appends inLines to the log view in their own colours and advances mLastSeq past the
        last of them. Keeps a live selection where it is rather than scrolling under it. */
    void _appendLines(const std::vector<quarry::ActivityLine>& inLines);

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
