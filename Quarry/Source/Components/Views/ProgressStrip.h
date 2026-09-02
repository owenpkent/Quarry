//
// The toolbar's own progress readout, filling the strip of background that used to sit empty
// above the transport (ProgressStripLayout says exactly where and why).
//

#ifndef ProgressStrip_h
#define ProgressStrip_h

#include "JuceHeader.h"

#include "TranscriptionManager.h"
#include "UIDefines.h"

/**
    Manager-independent, like ActivityDrawer: handed a provider for the stage to show and a
    callback for what Cancel means, so it can be built and reasoned about on its own.

    Hidden whenever the current stage is not active, and whenever the page it lives on is not
    showing (see setPageShowing) -- the strip has no way to know on its own which of
    QuarryMainView's two pages is up, and a take running behind the Sample page must not pop this
    over it.
*/
class ProgressStrip
    : public Component
    , public Timer
{
public:
    ProgressStrip(std::function<TranscriptionManager::Stage()> inStageProvider,
                 std::function<void()> inOnCancel);

    ~ProgressStrip() override;

    /** Windows' Sample/Transcribe split is the only reason this exists; on a single-page build
        it is simply never called and the strip behaves as if it were always showing. While
        false, the strip stays hidden regardless of what the stage says, so a job left running
        behind the Sample page cannot surface over it. */
    void setPageShowing(bool inShowing);

    void paint(Graphics& g) override;

    void resized() override;

    void timerCallback() override;

private:
    /** Applies inStage: shows/hides, repaints, and fires the accessibility events the transitions
        and the text (not the fraction) owe. Called from the timer, and once directly from
        setPageShowing so a page flip does not wait up to one tick to take effect. */
    void _applyStage(const TranscriptionManager::Stage& inStage);

    String _captionFor(const TranscriptionManager::Stage& inStage) const;

    void _paintBar(Graphics& g) const;

    std::function<TranscriptionManager::Stage()> mStageProvider;
    std::function<void()> mOnCancel;

    std::unique_ptr<TextButton> mCancelButton;

    bool mPageShowing = true;

    // Whether the timer is currently running at kActiveTimerHz rather than kIdleTimerHz -- see
    // the constants' own comment in ProgressStrip.cpp for why this can never just stop instead.
    // Tracked so _applyStage only calls startTimerHz on an actual change of rate.
    bool mTimerIsFast = false;

    TranscriptionManager::Stage mCurrentStage;

    // What the last _applyStage call already told the accessibility tree, so a call that changes
    // nothing announces nothing. Separate from each other: the caption (with its percentage)
    // moves on every tick of real progress, and setDescription tracks that; the announcement
    // itself is gated on the coarser stage text alone, so a running percentage does not narrate.
    String mLastCaption;
    String mLastAnnouncedText;

    // The unknown-fraction sweep: a segment's position in [0, 1) over the bar's own travel,
    // advanced in the timer while the strip is visible. See _paintBar.
    double mSweepPhase = 0.0;
    double mLastTimerMs = 0.0;

    Rectangle<int> mBarArea;
    Rectangle<int> mCaptionArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProgressStrip)
};

#endif // ProgressStrip_h
