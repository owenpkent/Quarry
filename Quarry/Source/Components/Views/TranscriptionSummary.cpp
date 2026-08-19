//
// What Quarry heard.
//

#include "TranscriptionSummary.h"

#include <algorithm>
#include <cmath>

#include <okstudio/Obsidian.h>

#include "NnId.h"
#include "NoteUtils.h"
#include "QuarryTooltips.h"
#include "SourceAudioManager.h"
#include "TranscriptionManager.h"

namespace
{
/** The narrowest a cell may be before bars start sharing one. Below this the strip stops
    being something you can point at, which is the only thing it is for. */
constexpr int minimumCellWidth = 9;

constexpr int cellGap = 2;

/** A clock, from seconds. Minutes and seconds, because takes are minutes long and an hour
    of buffered audio is not something this ever sees in one transcription. */
String asClock(double inSeconds)
{
    const auto whole = jmax(0, roundToInt(std::floor(inSeconds)));
    return String(whole / 60) + ":" + String(whole % 60).paddedLeft('0', 2);
}

/** Accent through amber to red, against two cut points worked out from the take's own middle.
    Colour is how a slump is found at a glance; which bar it is comes off the ruler under the
    strip and off the sentence below it, so nothing here is carried by hue alone. */
Colour tierColour(double inConfidence, double inUnsureBelow, double inShakyBelow, Colour inAccent)
{
    if (inConfidence < 0.0)
        return CONTROL_BG; // Nothing here to be sure or unsure about.

    if (inConfidence >= inUnsureBelow)
        return inAccent;

    if (inConfidence >= inShakyBelow)
        return Colour(0xffd9a441);

    return RECORD_RED;
}
} // namespace

TranscriptionSummary::TranscriptionSummary(QuarryAudioProcessor& inProcessor)
    : mProcessor(inProcessor)
{
    auto tempoIsValid = [](const String& inText) {
        if (inText.isEmpty())
            return false;

        const auto tempo = inText.getFloatValue();
        return tempo >= 20.0f && tempo <= 999.0f;
    };

    auto correctTempo = [](const String& inText) {
        return inText.isEmpty() ? String("120") : String(jlimit(20.0f, 999.0f, inText.getFloatValue()));
    };

    // The same editor that used to sit in the strip between the waveform and the roll, moved
    // in here beside the key and the meter: it is one of the things the take is described by,
    // not a control belonging to a band of its own.
    mTempo = std::make_unique<NumericTextEditor<double>>(
        &mProcessor, NnId::ExportTempoId, 6, 120.0, Justification::centredLeft, tempoIsValid, correctTempo);
    mTempo->setTooltip(QuarryTooltips::export_tempo);
    addAndMakeVisible(*mTempo);

    mUseKeyButton = std::make_unique<TextButton>("SNAP TO IT");
    mUseKeyButton->setColour(TextButton::buttonColourId, CONTROL_BG);
    mUseKeyButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mUseKeyButton->setTooltip(QuarryTooltips::use_detected_key);
    mUseKeyButton->onClick = [this]() { _adoptDetectedKey(); };
    addAndMakeVisible(*mUseKeyButton);

    mNextShakyButton = std::make_unique<TextButton>("NEXT SHAKY BAR");
    mNextShakyButton->setColour(TextButton::buttonColourId, CONTROL_BG);
    mNextShakyButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mNextShakyButton->setTooltip("Move the playhead to the next bar the model was least sure of.");
    mNextShakyButton->onClick = [this]() { _goToNextShakyBar(); };
    addAndMakeVisible(*mNextShakyButton);

    mRollButton = std::make_unique<TextButton>("show notes");
    mRollButton->setColour(TextButton::buttonColourId, Colours::transparentBlack);
    mRollButton->setColour(TextButton::textColourOffId, TEXT_DIM);
    mRollButton->setTooltip("Show the notes themselves, under the waveform.");
    mRollButton->onClick = [this]() {
        if (onRollVisibilityToggled != nullptr)
            onRollVisibilityToggled(! mRollVisible);
    };
    addAndMakeVisible(*mRollButton);

    clear();

    // Twice a second, which is what the key readout has always run at: the transcription is
    // rebuilt on a worker thread and there is no edge to listen for, only a revision to
    // notice. Nothing here animates, so there is nothing faster to be.
    startTimerHz(2);
}

TranscriptionSummary::~TranscriptionSummary()
{
    stopTimer();
}

int TranscriptionSummary::naturalHeight()
{
    return NATURAL_HEIGHT;
}

int TranscriptionSummary::compactHeight()
{
    return COMPACT_HEIGHT;
}

//==============================================================================
void TranscriptionSummary::resized()
{
    auto area = getLocalBounds().reduced(12, 8);

    mReadoutBounds = area.removeFromTop(56);

    // The two buttons stack in a column of their own on the right, so the readout keeps a run
    // of five even columns and nothing has to be laid out around a control.
    auto buttons = mReadoutBounds.removeFromRight(104);
    mRollButton->setBounds(buttons.removeFromTop(20));
    buttons.removeFromTop(6);
    mUseKeyButton->setBounds(buttons.removeFromTop(22));

    // The tempo is the second of the readout's five columns, and the one column whose value is
    // typed rather than reported. Placed off the same arithmetic _paintReadout uses, so the
    // editor lands under the label that names it.
    const auto columnWidth = mReadoutBounds.getWidth() / 5;
    mTempo->setBounds(mReadoutBounds.getX() + columnWidth, mReadoutBounds.getY() + 14,
                      jmin(70, jmax(30, columnWidth - 8)), 22);

    area.removeFromTop(8);

    // Not enough room for a strip anyone could read: the readout is the whole panel, and the
    // rest of the height is doing something better somewhere else.
    if (area.getHeight() < 60)
    {
        mNextShakyButton->setVisible(false);
        mStripLabelBounds = {};
        mStripBounds = {};
        mRulerBounds = {};
        mVerdictBounds = {};
        _rebuildBars();
        return;
    }

    mNextShakyButton->setVisible(true);

    auto label = area.removeFromTop(20);
    mNextShakyButton->setBounds(label.removeFromRight(130).withHeight(19));
    mStripLabelBounds = label;

    area.removeFromTop(4);

    // The verdict and the ruler are fixed; the strip takes what is between them, up to a cap.
    // Past that a taller cell says nothing more, and the space is better left as space.
    mVerdictBounds = area.removeFromBottom(18);
    area.removeFromBottom(4);

    auto forStrip = area;
    mStripBounds = forStrip.removeFromTop(jmin(72, jmax(18, forStrip.getHeight() - 16)));
    mRulerBounds = forStrip.removeFromTop(14);

    // The cells only exist once their width is known, so a resize is a rebuild.
    _rebuildBars();
}

void TranscriptionSummary::paint(Graphics& g)
{
    okstudio::obsidian::raisedFill(g, getLocalBounds().toFloat(), 4.0f, PANEL_TOP, PANEL_BOT);

    _paintReadout(g);
    _paintStrip(g);
}

//==============================================================================
void TranscriptionSummary::_paintReadout(Graphics& g)
{
    auto area = mReadoutBounds;

    // Five columns, laid out by share rather than by pixel so the block survives a window that
    // is not the width it was designed at.
    const auto columns = 5;
    const auto columnWidth = area.getWidth() / columns;

    const auto drawColumn = [&](int inIndex, const String& inLabel, const String& inValue, Colour inValueColour) {
        auto column = area.withX(area.getX() + inIndex * columnWidth).withWidth(columnWidth - 8);

        g.setColour(TEXT_DIM);
        g.setFont(UIDefines::LABEL_FONT());
        g.drawText(inLabel, column.removeFromTop(14), Justification::topLeft, false);

        g.setColour(inValueColour);
        g.setFont(Font(FontOptions(UIDefines::MONTSERRAT_SEMIBOLD())).withPointHeight(15.0f));
        g.drawText(inValue, column.removeFromTop(22), Justification::topLeft, true);
    };

    const auto accent = okstudio::obsidian::accentOf(*this).base;

    if (! mHasReading)
    {
        g.setColour(TEXT_DIM);
        g.setFont(UIDefines::LABEL_FONT());
        g.drawText("NOTHING TRANSCRIBED YET", mReadoutBounds, Justification::centredLeft, false);
        return;
    }

    // The runner-up under the winner, always. A key reading is a ranking, and printing only
    // the top of it turns a two-horse race into a statement of fact.
    const auto keyText = mKey.isValid() ? mKey.toString() : String("no clear key");
    drawColumn(0, "KEY", keyText, mKey.isValid() ? accent : TEXT_DIM);

    if (mKey.isValid())
    {
        g.setColour(TEXT_DIM);
        g.setFont(UIDefines::LABEL_FONT());
        g.drawText("or " + mKey.runnerUpToString() + "   " + String(mKey.confidence, 2),
                   mReadoutBounds.withY(mReadoutBounds.getY() + 38).withHeight(16).withWidth(columnWidth - 8),
                   Justification::topLeft,
                   false);
    }

    // Column one is the tempo, whose value is an editor rather than text, so only its label
    // is drawn here.
    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("TEMPO", area.withX(area.getX() + columnWidth).withWidth(columnWidth - 8).withHeight(14),
               Justification::topLeft, false);

    const auto numerator = (int) mProcessor.getValueTree().getProperty(NnId::TimeSignatureNumeratorId, 4);
    const auto denominator = (int) mProcessor.getValueTree().getProperty(NnId::TimeSignatureDenominatorId, 4);

    drawColumn(2, "METER", String(numerator) + " / " + String(denominator), TEXT_MAIN);
    drawColumn(3, "NOTES", String(mNoteCount), TEXT_MAIN);
    drawColumn(4, "LENGTH", asClock(_takeSeconds()), TEXT_MAIN);
}

void TranscriptionSummary::_paintStrip(Graphics& g)
{
    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());

    if (! mHasReading || mStripBounds.isEmpty())
        return;

    if (! mHasConfidence)
    {
        g.drawText("CONFIDENCE BY BAR", mStripLabelBounds, Justification::centredLeft, false);
        g.drawFittedText("This engine does not report per-note confidence, so there is nothing "
                         "honest to draw here.",
                         mStripBounds.withHeight(jmin(mStripBounds.getHeight(), 34)),
                         Justification::topLeft,
                         2);
        return;
    }

    g.drawText("CONFIDENCE BY BAR", mStripLabelBounds, Justification::centredLeft, false);

    if (mBars.empty() || mStripBounds.getWidth() <= 0)
        return;

    const auto accent = okstudio::obsidian::accentOf(*this).base;
    const auto cells = (int) ((mBars.size() + (size_t) mBarsPerCell - 1) / (size_t) mBarsPerCell);
    const auto cellWidth = (double) mStripBounds.getWidth() / (double) cells;

    for (int cell = 0; cell < cells; cell++)
    {
        // The worst bar in the cell, not the mean of them. A cell standing for four bars is a
        // pointer to somewhere worth looking, and averaging a bad bar away would hide exactly
        // what it exists to find.
        auto worst = -1.0;
        auto anyNotes = false;

        for (int b = cell * mBarsPerCell; b < jmin((int) mBars.size(), (cell + 1) * mBarsPerCell); b++)
        {
            if (mBars[(size_t) b].confidence < 0.0)
                continue;

            anyNotes = true;
            worst = worst < 0.0 ? mBars[(size_t) b].confidence : jmin(worst, mBars[(size_t) b].confidence);
        }

        auto bounds = Rectangle<double>(mStripBounds.getX() + cell * cellWidth,
                                        (double) mStripBounds.getY(),
                                        cellWidth - cellGap,
                                        (double) mStripBounds.getHeight())
                          .toFloat();

        g.setColour(tierColour(anyNotes ? worst : -1.0,
                               mMedianConfidence * kUnsureBelowMedian,
                               mMedianConfidence * kShakyBelowMedian,
                               accent));
        g.fillRoundedRectangle(bounds, 2.0f);

        if (mHoveredBar >= 0 && mHoveredBar / mBarsPerCell == cell)
        {
            g.setColour(TEXT_MAIN.withAlpha(0.6f));
            g.drawRoundedRectangle(bounds.reduced(0.5f), 2.0f, 1.0f);
        }
    }

    // The ruler: first, last, and the round numbers between them that fit. Every cell numbered
    // was a row of digits nobody read.
    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());

    const auto drawTick = [&](int inBar) {
        const auto cell = inBar / mBarsPerCell;
        const auto x = mStripBounds.getX() + (int) (cell * cellWidth);

        g.drawText(String(inBar + 1), Rectangle<int>(x, mRulerBounds.getY(), 40, mRulerBounds.getHeight()),
                   Justification::topLeft, false);
    };

    const auto step = jmax(1, roundToInt(std::ceil(40.0 / (cellWidth * mBarsPerCell))) * mBarsPerCell);

    for (int bar = 0; bar < (int) mBars.size(); bar += step)
        drawTick(bar);

    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText(_verdictText(), mVerdictBounds, Justification::centredLeft, true);
}

//==============================================================================
void TranscriptionSummary::timerCallback()
{
    _refresh();
}

void TranscriptionSummary::_refresh()
{
    auto* manager = mProcessor.getTranscriptionManager();

    // The transcription job rebuilds the note vector on a worker thread, and the store of this
    // state at the end of that job is the only thing that makes it safe to read from here.
    if (mProcessor.getState() != PopulatedAudioAndMidiRegions)
    {
        if (mHasReading)
            clear();

        return;
    }

    const auto revision = manager->getRawNoteEventsRevision();

    if (mHasReading && revision == mLastNoteRevision)
        return;

    mLastNoteRevision = revision;
    mHasReading = true;

    // Judged on the model output rather than on what scale quantize left behind, for the same
    // reason the key readout always was: with SNAP MODE set to Remove, every out-of-key note
    // is already gone and the reading would do no more than repeat what the user typed in.
    const auto& events = manager->getRawNoteEventVector();

    mKey = estimateKey(events);
    mNoteCount = (int) events.size();

    mHasConfidence = std::any_of(events.begin(), events.end(),
                                 [](const Notes::Event& event) { return event.amplitude > 0.0; });

    _rebuildBars();

    mUseKeyButton->setVisible(mKey.isValid());
    mNextShakyButton->setEnabled(mHasConfidence);

    repaint();
}

void TranscriptionSummary::clear()
{
    mHasReading = false;
    mLastNoteRevision = 0;
    mKey = KeyEstimate();
    mNoteCount = 0;
    mHasConfidence = false;
    mHoveredBar = -1;
    mMedianConfidence = -1.0;
    mBars.clear();

    mUseKeyButton->setVisible(false);
    mNextShakyButton->setEnabled(false);

    repaint();
}

void TranscriptionSummary::setRollVisible(bool inIsVisible)
{
    mRollVisible = inIsVisible;
    mRollButton->setButtonText(inIsVisible ? "hide notes" : "show notes");
}

//==============================================================================
void TranscriptionSummary::_rebuildBars()
{
    mBars.clear();
    mBarsPerCell = 1;
    mMedianConfidence = -1.0;

    if (! mHasReading)
        return;

    const double tempo = mProcessor.getValueTree().getProperty(NnId::ExportTempoId, 120.0);
    const int numerator = (int) mProcessor.getValueTree().getProperty(NnId::TimeSignatureNumeratorId, 4);
    const int denominator = (int) mProcessor.getValueTree().getProperty(NnId::TimeSignatureDenominatorId, 4);

    if (tempo <= 0.0 || numerator <= 0 || denominator <= 0)
        return;

    // A bar is `numerator` beats of `denominator`, and the tempo is quarter notes per minute,
    // which is what makes the 4/denominator in here rather than a 1.
    const auto secondsPerBar = (60.0 / tempo) * numerator * (4.0 / denominator);

    const auto seconds = _takeSeconds();

    if (secondsPerBar <= 0.0 || seconds <= 0.0)
        return;

    const auto count = jlimit(1, 4096, (int) std::ceil(seconds / secondsPerBar));

    mBars.resize((size_t) count);

    std::vector<double> sums((size_t) count, 0.0);

    for (size_t i = 0; i < mBars.size(); i++)
        mBars[i].startSeconds = (double) i * secondsPerBar;

    for (const auto& event : mProcessor.getTranscriptionManager()->getRawNoteEventVector())
    {
        const auto bar = (int) (event.startTime / secondsPerBar);

        if (! isPositiveAndBelow(bar, count))
            continue;

        sums[(size_t) bar] += event.amplitude;
        mBars[(size_t) bar].noteCount++;
    }

    for (size_t i = 0; i < mBars.size(); i++)
        if (mBars[i].noteCount > 0)
            mBars[i].confidence = sums[i] / mBars[i].noteCount;

    mMedianConfidence = _medianConfidence();

    // However many bars have to share a cell for the cells to stay wide enough to point at.
    if (mStripBounds.getWidth() > 0)
    {
        const auto affordable = jmax(1, mStripBounds.getWidth() / minimumCellWidth);
        mBarsPerCell = jmax(1, (count + affordable - 1) / affordable);
    }
}

double TranscriptionSummary::_medianConfidence() const
{
    std::vector<double> scores;

    for (const auto& bar : mBars)
        if (bar.confidence >= 0.0)
            scores.push_back(bar.confidence);

    if (scores.empty())
        return -1.0;

    // The median rather than the mean, because one bar of noise at the end of a take drags a
    // mean far enough to retint the whole strip.
    std::sort(scores.begin(), scores.end());
    return scores[scores.size() / 2];
}

double TranscriptionSummary::_takeSeconds() const
{
    return mProcessor.getSourceAudioManager()->getAudioSampleDuration();
}

String TranscriptionSummary::_verdictText() const
{
    if (! mHasConfidence || mBars.empty() || mMedianConfidence <= 0.0)
        return {};

    std::vector<int> shaky;
    std::vector<int> soft;
    auto withNotes = 0;

    for (size_t i = 0; i < mBars.size(); i++)
    {
        if (mBars[i].confidence < 0.0)
            continue;

        withNotes++;

        if (mBars[i].confidence < mMedianConfidence * kShakyBelowMedian)
            shaky.push_back((int) i + 1);
        else if (mBars[i].confidence < mMedianConfidence * kUnsureBelowMedian)
            soft.push_back((int) i + 1);
    }

    if (withNotes == 0)
        return "No notes found in this take.";

    // Three names and then a count. A sentence listing eleven bar numbers is a sentence nobody
    // finishes, and the strip above is already the complete answer.
    const auto name = [](const std::vector<int>& inBars) {
        String named;

        for (size_t i = 0; i < jmin((size_t) 3, inBars.size()); i++)
            named += (i == 0 ? "" : ", ") + String(inBars[i]);

        if (inBars.size() > 3)
            named += " and " + String((int) inBars.size() - 3) + " more";

        return named;
    };

    // The sentence says what the strip shows, tier for tier. Saying "even" over a strip with
    // eight bars tinted in it was the readout arguing with itself.
    if (! shaky.empty())
        return "Bar" + String(shaky.size() == 1 ? " " : "s ") + name(shaky)
             + String(shaky.size() == 1 ? " is" : " are") + " the least certain of the "
             + String(withNotes) + " - worth a look.";

    if (! soft.empty())
        return "Mostly even. Bar" + String(soft.size() == 1 ? " " : "s ") + name(soft)
             + String(soft.size() == 1 ? " sits" : " sit") + " a little below the rest.";

    return "Confidence is even across all " + String(withNotes) + " bars.";
}

//==============================================================================
int TranscriptionSummary::_barAt(Point<int> inPoint) const
{
    if (mBars.empty() || ! mStripBounds.contains(inPoint))
        return -1;

    const auto cells = (int) ((mBars.size() + (size_t) mBarsPerCell - 1) / (size_t) mBarsPerCell);
    const auto cellWidth = (double) mStripBounds.getWidth() / (double) cells;

    const auto cell = (int) ((inPoint.x - mStripBounds.getX()) / cellWidth);

    return isPositiveAndBelow(cell * mBarsPerCell, (int) mBars.size()) ? cell * mBarsPerCell : -1;
}

void TranscriptionSummary::mouseDown(const MouseEvent& inEvent)
{
    const auto bar = _barAt(inEvent.getPosition());

    if (bar >= 0)
        _goToBar(bar);
}

void TranscriptionSummary::mouseMove(const MouseEvent& inEvent)
{
    const auto bar = _barAt(inEvent.getPosition());

    if (bar == mHoveredBar)
        return;

    mHoveredBar = bar;

    if (bar < 0 || ! mHasConfidence)
    {
        setTooltip({});
    }
    else
    {
        const auto& hovered = mBars[(size_t) bar];

        setTooltip(hovered.noteCount == 0
                       ? "Bar " + String(bar + 1) + ": no notes"
                       : "Bar " + String(bar + 1) + ": " + String(hovered.noteCount) + " notes, confidence "
                             + String(hovered.confidence, 2));
    }

    repaint(mStripBounds);
}

void TranscriptionSummary::mouseExit(const MouseEvent&)
{
    if (mHoveredBar < 0)
        return;

    mHoveredBar = -1;
    setTooltip({});
    repaint(mStripBounds);
}

void TranscriptionSummary::_goToBar(int inBar)
{
    if (! isPositiveAndBelow(inBar, (int) mBars.size()))
        return;

    mProcessor.getPlayer()->setPlayheadPositionSeconds(mBars[(size_t) inBar].startSeconds);
}

void TranscriptionSummary::_goToNextShakyBar()
{
    if (mBars.empty() || ! mHasConfidence)
        return;

    const auto here = mProcessor.getPlayer()->getPlayheadPositionSeconds();

    // Anything the strip tinted, not only the worst tier: a take whose slump is mild still has
    // bars worth checking, and a button that does nothing on it teaches you to stop pressing it.
    const auto isShaky = [this](size_t inIndex) {
        return mBars[inIndex].confidence >= 0.0
            && mBars[inIndex].confidence < mMedianConfidence * kUnsureBelowMedian;
    };

    // Forwards from where the playhead is, then round to the top, so pressing it repeatedly
    // walks the whole take rather than sticking at the last one.
    for (size_t i = 0; i < mBars.size(); i++)
        if (isShaky(i) && mBars[i].startSeconds > here + 0.001)
            return _goToBar((int) i);

    for (size_t i = 0; i < mBars.size(); i++)
        if (isShaky(i))
            return _goToBar((int) i);
}

void TranscriptionSummary::_adoptDetectedKey()
{
    if (! mKey.isValid())
        return;

    const auto set = [this](ParameterHelpers::ParamIdEnum inId, int inIndex, int inCount) {
        auto* parameter = mProcessor.getParams()[static_cast<size_t>(inId)];
        parameter->setValueNotifyingHost((float) inIndex / (float) jmax(1, inCount - 1));
    };

    // The estimator counts from C; the picker starts at A.
    set(ParameterHelpers::KeyRootNoteId, (mKey.rootNote + 3) % 12, 12);
    set(ParameterHelpers::KeyTypeId, mKey.isMinor ? NoteUtils::Minor : NoteUtils::Major,
        NoteUtils::ScaleTypesStr.size());
}
