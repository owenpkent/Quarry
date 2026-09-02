//
// The dev-facing overlay that shows what Quarry and the sidecar have been doing.
//

#include "ActivityDrawer.h"

#include "ActivityDrawerLayout.h"
#include "ActivityFormat.h"
#include "QuarryLookAndFeel.h"

#include <okstudio/Obsidian.h>

namespace
{
// The prompt's "chevron, then a space" prefix. Written as UTF-8 bytes rather than a literal
// character in the source: MSVC reads a source file's non-ASCII bytes according to whatever
// codepage the machine is set to unless told otherwise, and a byte-escaped CharPointer_UTF8
// sidesteps that entirely.
const String promptGlyph(CharPointer_UTF8("\xE2\x80\xBA"));

constexpr int kCloseButtonSize = 18;
constexpr int kPromptGlyphWidth = 16;
} // namespace

ActivityDrawer::ActivityDrawer(quarry::ActivityLog& inLog,
                               std::function<String()> inStatusText,
                               std::function<void(const String&)> inOnCommand)
    : mLog(inLog)
    , mStatusText(std::move(inStatusText))
    , mOnCommand(std::move(inOnCommand))
{
    const auto monoFont = [](float inHeight) {
        return Font(FontOptions(Font::getDefaultMonospacedFontName(), inHeight, Font::plain));
    };

    mLogView = std::make_unique<TextEditor>("ActivityLog");
    mLogView->setMultiLine(true, true);
    mLogView->setReturnKeyStartsNewLine(false);
    mLogView->setReadOnly(true);
    mLogView->setCaretVisible(false);
    mLogView->setScrollbarsShown(true);
    mLogView->setFont(monoFont(12.0f));
    mLogView->setColour(TextEditor::backgroundColourId, PANEL_BOT);
    mLogView->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    mLogView->setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
    mLogView->setColour(TextEditor::textColourId, TEXT_MAIN);
    // A screen reader has no use for a line arriving four times a second -- that is the
    // read-out equivalent of a log window stealing focus on every append -- so nothing here
    // calls an announcement API. The title and description are what say what this control is
    // when something does ask it directly.
    mLogView->setTitle("Activity log");
    mLogView->setDescription("A running log of what Quarry and the sidecar have been doing.");
    addAndMakeVisible(*mLogView);

    mPrompt = std::make_unique<TextEditor>("ActivityPrompt");
    mPrompt->setMultiLine(false);
    mPrompt->setReturnKeyStartsNewLine(false);
    mPrompt->setFont(monoFont(12.0f));
    mPrompt->setColour(TextEditor::backgroundColourId, Colours::transparentBlack);
    mPrompt->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    mPrompt->setColour(TextEditor::focusedOutlineColourId, Colours::transparentBlack);
    mPrompt->setColour(TextEditor::textColourId, TEXT_MAIN);
    mPrompt->setTextToShowWhenEmpty("paste a URL and press Enter", TEXT_DIM);
    mPrompt->setTitle("Activity command");
    mPrompt->setDescription("Paste a URL to download it, or type clear to clear the log.");
    mPrompt->onReturnKey = [this]() { _handleReturn(); };
    mPrompt->onEscapeKey = [this]() { mPrompt->giveAwayKeyboardFocus(); };
    addAndMakeVisible(*mPrompt);

    mCloseButton = std::make_unique<DrawableButton>("CloseActivity", DrawableButton::ButtonStyle::ImageFitted);
    mCloseButton->setColour(DrawableButton::ColourIds::backgroundColourId, Colours::transparentBlack);
    mCloseButton->setColour(DrawableButton::ColourIds::backgroundOnColourId, Colours::transparentBlack);
    auto close_icon = quarry::lnf::icon(okstudio::icons::close, TEXT_DIM);
    mCloseButton->setImages(close_icon.get());
    mCloseButton->setTitle("Close activity log");
    mCloseButton->setTooltip("Close the activity log.");
    mCloseButton->onClick = [this]() { close(); };
    addAndMakeVisible(*mCloseButton);

    setWantsKeyboardFocus(false);
    setVisible(false);
}

ActivityDrawer::~ActivityDrawer()
{
    stopTimer();
}

void ActivityDrawer::open()
{
    setVisible(true);
    toFront(true);
    mPrompt->grabKeyboardFocus();
}

void ActivityDrawer::close()
{
    setVisible(false);

    if (onClosed != nullptr)
        onClosed();
}

void ActivityDrawer::visibilityChanged()
{
    if (isVisible()) {
        _rebuildFromSnapshot();
        // Cleared so the next tick's status differs from whatever is stale from before the
        // drawer was last hidden, and repaints the header immediately rather than leaving last
        // session's sentence on screen for up to 100ms.
        mLastStatus.clear();
        startTimerHz(10);
        timerCallback();
    } else {
        stopTimer();
    }
}

void ActivityDrawer::timerCallback()
{
    const auto revision = mLog.revision();

    if (revision != mLastSeq)
        _appendLines(mLog.linesSince(mLastSeq));

    if (mStatusText != nullptr) {
        const auto status = mStatusText();

        if (status != mLastStatus) {
            mLastStatus = status;
            repaint(mHeaderTextArea);
        }
    }
}

void ActivityDrawer::_appendLines(const std::vector<quarry::ActivityLine>& inLines)
{
    if (inLines.empty())
        return;

    // A selection is the reader mid-copy; scrolling out from under it would lose their place; a
    // caret is just insertion point bookkeeping we would move past their input anyway.
    const auto selection = mLogView->getHighlightedRegion();
    const bool has_selection = !selection.isEmpty();

    mLogView->setCaretPosition(mLogView->getTotalNumChars());

    for (const auto& line: inLines) {
        mLogView->setColour(TextEditor::textColourId, _colourFor(line.kind));
        mLogView->insertTextAtCaret(quarry::formatActivityLine(line) + "\n");
        mLastSeq = line.seq;
    }

    if (has_selection)
        mLogView->setHighlightedRegion(selection);
    else
        mLogView->setCaretPosition(mLogView->getTotalNumChars());
}

void ActivityDrawer::_rebuildFromSnapshot()
{
    mLogView->clear();
    mLastSeq = 0;

    _appendLines(mLog.snapshot());
}

Colour ActivityDrawer::_colourFor(quarry::ActivityLine::Kind inKind) const
{
    switch (inKind) {
    case quarry::ActivityLine::Kind::Quarry: return TEXT_MAIN;
    case quarry::ActivityLine::Kind::Stage: return okstudio::obsidian::accentOf(*this).hot;
    case quarry::ActivityLine::Kind::Stderr: return TEXT_DIM;
    case quarry::ActivityLine::Kind::Error: return quarry::lnf::DESTRUCTIVE;
    }

    return TEXT_MAIN;
}

void ActivityDrawer::_handleReturn()
{
    const auto text = mPrompt->getText().trim();

    if (text.isEmpty())
        return;

    if (text.equalsIgnoreCase("clear")) {
        mLog.clear();
        mLogView->clear();
        mLastSeq = 0;
    } else if (mOnCommand != nullptr) {
        mOnCommand(text);
    }

    mPrompt->setText({}, dontSendNotification);
}

void ActivityDrawer::paint(Graphics& g)
{
    // The section fill every other panel uses, covering whatever the left column and the
    // footer are showing underneath.
    okstudio::obsidian::raisedFill(g, getLocalBounds().toFloat(), 5.0f, PANEL_TOP, PANEL_BOT);

    // A low-alpha top edge on top of raisedFill's own catch-light, so the panel reads as
    // something that slid up over the window rather than a section that was always there.
    g.setColour(TEXT_DIM.withAlpha(0.25f));
    g.drawHorizontalLine(0, 0.0f, (float) getWidth());

    g.setFont(UIDefines::TITLE_FONT());
    g.setColour(TEXT_MAIN);
    g.drawText("ACTIVITY", mHeaderTextArea, Justification::centredLeft);

    g.setFont(UIDefines::LABEL_FONT());
    g.setColour(TEXT_DIM);
    g.drawText(mLastStatus, mHeaderTextArea, Justification::centredRight);

    g.setFont(mMonoPromptFont);
    g.setColour(TEXT_DIM);
    g.drawText(promptGlyph, mPromptGlyphArea, Justification::centred);
}

void ActivityDrawer::resized()
{
    namespace L = ActivityDrawerLayout;

    auto area = getLocalBounds();
    area.removeFromLeft(L::PAD);
    area.removeFromRight(L::PAD);
    area.removeFromTop(L::PAD);

    auto header = area.removeFromTop(L::HEADER);
    mCloseButton->setBounds(
        header.removeFromRight(kCloseButtonSize).withSizeKeepingCentre(kCloseButtonSize, kCloseButtonSize));
    header.removeFromRight(L::PAD);
    mHeaderTextArea = header;

    area.removeFromTop(L::PAD);
    mLogView->setBounds(area.removeFromTop(L::logHeight()));

    area.removeFromTop(L::PAD);
    auto prompt_row = area.removeFromTop(L::PROMPT);
    mPromptGlyphArea = prompt_row.removeFromLeft(kPromptGlyphWidth);
    mPrompt->setBounds(prompt_row);
}
