//
// The toolbar's own progress readout: the same stage feed the activity drawer shows, for whoever
// never opens the drawer.
//

#include "ProgressStrip.h"

#include "ProgressStripLayout.h"

#include <okstudio/Obsidian.h>

#include <cmath>

namespace
{
// 15 Hz, and only while a stage is actually showing: fast enough for the sweep and a moving
// percentage to read as live.
//
// The strip used to run this timer from construction to destruction, which meant waking the
// message thread fifteen times a second to take the manager's stage lock and copy a Stage --
// a juce::String included -- for what is overwhelmingly the whole life of an open editor: time
// with no job running at all. It cannot simply stop while hidden the way ActivityDrawer's can,
// because nothing else would be left to notice the next job start.
//
// So the noticing moved out. QuarryMainView already runs a 30 Hz timer and already owns this
// strip; it calls pollStage() from there, and pollStage compares a lock-free revision counter
// before it copies anything. An idle strip now costs one relaxed atomic load per frame on a
// thread that was waking anyway, and still sees a job start within a frame rather than the
// quarter-second a slower timer of its own would have cost.
constexpr int kActiveTimerHz = 15;

// The unknown-fraction sweep: how long one pass of the lit segment takes, and how wide it is.
// Slow enough to read as "busy" rather than "alarmed", wide enough to be seen at 160 px.
constexpr double kSweepPeriodMs = 1200.0;
constexpr int kSweepWidth = 40;
} // namespace

ProgressStrip::ProgressStrip(std::function<TranscriptionManager::Stage()> inStageProvider,
                             std::function<std::uint32_t()> inRevisionProvider,
                             std::function<void()> inOnCancel)
    : mStageProvider(std::move(inStageProvider))
    , mRevisionProvider(std::move(inRevisionProvider))
    , mOnCancel(std::move(inOnCancel))
{
    mCancelButton = std::make_unique<TextButton>("CANCEL");
    mCancelButton->setColour(TextButton::buttonColourId, PANEL_BG);
    mCancelButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mCancelButton->setTitle("Cancel the running job");
    mCancelButton->setTooltip("Stop the sidecar's current take or download.");
    mCancelButton->onClick = [this]() {
        if (mOnCancel != nullptr)
            mOnCancel();
    };
    addChildComponent(*mCancelButton);

    setTitle("Transcription progress");
    setDescription("Idle.");
    setWantsKeyboardFocus(false);
    setVisible(false);

    mLastTimerMs = Time::getMillisecondCounterHiRes();

    // No timer here. It starts when a stage does; see pollStage.
}

ProgressStrip::~ProgressStrip()
{
    stopTimer();
}

void ProgressStrip::setPageShowing(bool inShowing)
{
    mPageShowing = inShowing;
    _applyStage(mCurrentStage);
}

void ProgressStrip::pollStage()
{
    if (mRevisionProvider == nullptr || mStageProvider == nullptr)
        return;

    const auto revision = mRevisionProvider();

    // The early out that makes this cheap enough to call every frame. The first poll always goes
    // through, so an editor opened onto a job already in flight shows the strip immediately
    // rather than waiting for the stage to happen to change again.
    if (mPolledOnce && revision == mLastRevision)
        return;

    mPolledOnce = true;
    mLastRevision = revision;

    _applyStage(mStageProvider());
}

void ProgressStrip::timerCallback()
{
    // Runs only while the strip is visible, and only to drive the sweep: what the stage says is
    // pushed in by pollStage now, not pulled here.
    const auto now_ms = Time::getMillisecondCounterHiRes();
    const auto elapsed_ms = now_ms - mLastTimerMs;
    mLastTimerMs = now_ms;

    if (isVisible() && mCurrentStage.fraction < 0.0) {
        mSweepPhase = std::fmod(mSweepPhase + elapsed_ms / kSweepPeriodMs, 1.0);
        repaint(mBarArea);
    }
}

void ProgressStrip::_applyStage(const TranscriptionManager::Stage& inStage)
{
    const bool was_visible = isVisible();
    const bool show = inStage.active && mPageShowing;
    const bool fraction_changed = inStage.fraction != mCurrentStage.fraction;

    mCurrentStage = inStage;

    if (show != was_visible)
        setVisible(show);

    // The sweep timer lives exactly as long as the strip is on screen. Guarded on the change
    // rather than called every time through, so a running job does not restart its own timer
    // on every stage update.
    if (show != was_visible) {
        if (show) {
            mLastTimerMs = Time::getMillisecondCounterHiRes();
            startTimerHz(kActiveTimerHz);
        } else {
            stopTimer();
        }
    }

    if (!show) {
        if (was_visible) {
            // The one announcement a finished job owes: that it is over. Reset the text gate too,
            // so the next job's first stage is announced even if it happens to read the same.
            mLastCaption.clear();
            mLastAnnouncedText.clear();
            setDescription("Idle.");

            if (auto* handler = getAccessibilityHandler())
                handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
        }

        return;
    }

    mCancelButton->setVisible(inStage.cancellable);

    const auto caption = _captionFor(inStage);

    if (caption != mLastCaption) {
        mLastCaption = caption;
        setDescription(caption);
        repaint(mCaptionArea);
    }

    if (fraction_changed)
        repaint(mBarArea);

    // Announced on the stage text, not the caption: the caption carries a running percentage,
    // and a screen reader narrating every few percent is the log-window-stealing-focus problem
    // ACCESSIBILITY.md warns about, in audio.
    if (!was_visible || inStage.text != mLastAnnouncedText) {
        mLastAnnouncedText = inStage.text;

        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent(AccessibilityEvent::valueChanged);
    }
}

String ProgressStrip::_captionFor(const TranscriptionManager::Stage& inStage) const
{
    auto caption = inStage.text.isNotEmpty() ? inStage.text : String("working");

    if (inStage.fraction >= 0.0)
        caption += "  " + String(roundToInt(jlimit(0.0, 1.0, inStage.fraction) * 100.0)) + "%";

    return caption;
}

void ProgressStrip::paint(Graphics& g)
{
    _paintBar(g);

    g.setFont(UIDefines::LABEL_FONT());
    g.setColour(TEXT_DIM);
    g.drawText(mLastCaption, mCaptionArea, Justification::centredLeft, true);
}

void ProgressStrip::_paintBar(Graphics& g) const
{
    const auto bar = mBarArea.toFloat();
    const auto radius = bar.getHeight() * 0.5f;

    // The track: the same recessed tone the panels use, with a faint edge so an empty bar still
    // reads as a bar and not as a smudge.
    g.setColour(PANEL_BOT);
    g.fillRoundedRectangle(bar, radius);
    g.setColour(TEXT_DIM.withAlpha(0.35f));
    g.drawRoundedRectangle(bar, radius, 1.0f);

    Rectangle<float> lit;

    if (mCurrentStage.fraction >= 0.0) {
        const auto width = bar.getWidth() * static_cast<float>(jlimit(0.0, 1.0, mCurrentStage.fraction));
        lit = bar.withWidth(width);
    } else {
        // Known-nothing: a segment sweeping the track, entering from the left and leaving off the
        // right, so it never looks parked at either end.
        const auto travel = bar.getWidth() + static_cast<float>(kSweepWidth);
        const auto x = bar.getX() - static_cast<float>(kSweepWidth) + static_cast<float>(mSweepPhase) * travel;
        lit = Rectangle<float>(x, bar.getY(), static_cast<float>(kSweepWidth), bar.getHeight()).getIntersection(bar);
    }

    if (lit.getWidth() <= 0.0f)
        return;

    const auto accent = okstudio::obsidian::accentOf(*this);

    g.setColour(accent.base);
    g.fillRoundedRectangle(lit, radius);
    g.setColour(accent.hot);
    g.fillRoundedRectangle(lit.reduced(0.0f, 2.0f), 1.0f);
}

void ProgressStrip::resized()
{
    namespace L = ProgressStripLayout;

    auto area = getLocalBounds();

    mBarArea = area.removeFromLeft(L::BAR_WIDTH).withSizeKeepingCentre(L::BAR_WIDTH, L::BAR_HEIGHT);
    area.removeFromLeft(L::GAP);

    const auto cancel = area.removeFromRight(L::CANCEL_WIDTH);
    mCancelButton->setBounds(cancel.withSizeKeepingCentre(L::CANCEL_WIDTH, L::CANCEL_HEIGHT));
    area.removeFromRight(L::GAP);

    mCaptionArea = area;
}
