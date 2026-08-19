//
// What Quarry heard, said in words and numbers rather than drawn as a grid.
//

#ifndef TranscriptionSummary_h
#define TranscriptionSummary_h

#include <cstdint>

#include <JuceHeader.h>

#include "KeyEstimate.h"
#include "NumericTextEditor.h"
#include "PluginProcessor.h"
#include "UIDefines.h"

/**
 * The block under the waveform: key, tempo, meter, how many notes, and which bars the model
 * was unsure about.
 *
 * This is what stands where the piano roll used to. The roll drew eighty-eight lanes of
 * mostly nothing and answered no question you could act on: the notes are not editable here,
 * they are going to a host, and staring at rectangles tells you neither whether the reading
 * is right nor where to go and fix it. The one thing the model knows that nothing downstream
 * can work out for itself is how sure it was, and that is what this shows.
 *
 * The strip is per bar rather than per note on purpose. A note is not a unit of work; a bar
 * is. "These three bars need a look" is an instruction, and "this note scored 0.63" is not.
 *
 * The roll is still reachable, and the toggle for it lives here.
 */
class TranscriptionSummary
    : public Component
    , public SettableTooltipClient
    , public Timer
{
public:
    explicit TranscriptionSummary(QuarryAudioProcessor& inProcessor);

    ~TranscriptionSummary() override;

    void resized() override;

    void paint(Graphics& g) override;

    void timerCallback() override;

    void mouseDown(const MouseEvent& inEvent) override;

    void mouseMove(const MouseEvent& inEvent) override;

    void mouseExit(const MouseEvent& inEvent) override;

    /** Back to the state before anything has been transcribed. */
    void clear();

    /** Told, not asked: whoever owns the roll decides whether it is up, and says so here so
        the button can name what it will do next. */
    void setRollVisible(bool inIsVisible);

    /** Fired by the roll toggle, with what the new state should be. */
    std::function<void(bool)> onRollVisibilityToggled;

    /** The height this needs to say everything it has to say. */
    static int naturalHeight();

    /** The readout alone, with the strip left off. What it is given when the roll is up: the
        roll wants the height more than a second opinion about the same take does, and the
        line that names the key and the tempo is the half you still want on screen. */
    static int compactHeight();

private:
    /** One bar of the take, and how sure the model was about what it found there. */
    struct Bar
    {
        double startSeconds = 0.0;

        /** Mean note-posteriorgram value over the notes starting in this bar, or -1 when no
            note starts here. Not zero: a bar with nothing in it is not a bar the model got
            wrong, and colouring it as one would send you to look at silence. */
        double confidence = -1.0;

        int noteCount = 0;
    };

    /** How far below the take's own middle a bar has to sit before it is worth a look.

        Relative, and it took a wrong turn to get here. Absolute cutoffs were tried first and
        are not available: the decoder derives its thresholds per take, so a dense polyphonic
        capture lands its whole distribution well under a number chosen for a solo line, and
        the first take run through this painted all thirty-one bars red. A readout that calls
        every bar bad is worth less than no readout.

        What survives calibration is the ranking. These are fractions of the take's own median
        bar, so a take whose confidence is even shows as even - nothing is tinted merely for
        being in the bottom quarter of a tight spread - and a real slump still stands out. The
        raw number is on each bar's tooltip, because a relative reading should always be
        checkable against the absolute one. */
    static constexpr double kUnsureBelowMedian = 0.93;
    static constexpr double kShakyBelowMedian = 0.85;

    /** The natural height of everything below: the readout, the strip at a readable size, the
        ruler and the verdict. The panel is given exactly this and the waveform above takes
        what is left, because a summary stretched to fill is a summary with a hole in it. */
    static constexpr int NATURAL_HEIGHT = 212;

    static constexpr int COMPACT_HEIGHT = 76;

    void _refresh();

    /** Rebuilds mBars from the note events, the export tempo and the meter. */
    void _rebuildBars();

    /** The median confidence over the bars that have notes, or -1 when none do. The yardstick
        every tier below is measured against. */
    double _medianConfidence() const;

    /** The bar under a point, or -1. */
    int _barAt(Point<int> inPoint) const;

    /** Moves the playhead to a bar and, when the roll is up, scrolls it into view. */
    void _goToBar(int inBar);

    void _goToNextShakyBar();

    void _adoptDetectedKey();

    /** Seconds of audio the take holds, or 0. */
    double _takeSeconds() const;

    /** "11 of 14 bars look solid. Bars 6, 7 and 11 are worth a look." */
    String _verdictText() const;

    void _paintReadout(Graphics& g);

    void _paintStrip(Graphics& g);

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<NumericTextEditor<double>> mTempo;
    std::unique_ptr<TextButton> mUseKeyButton;
    std::unique_ptr<TextButton> mNextShakyButton;
    std::unique_ptr<TextButton> mRollButton;

    KeyEstimate mKey;

    std::vector<Bar> mBars;

    /** How many bars share one cell of the strip. One until a long take runs out of pixels,
        because a cell narrower than a finger is a cell nobody can click. */
    int mBarsPerCell = 1;

    int mNoteCount = 0;

    /** False when every note came back with a confidence of zero, which is what an engine
        that does not report one looks like. The sidecar is that engine, and a strip painted
        from its silence would be a solid block of red. */
    bool mHasConfidence = false;

    /** Recomputed with mBars, because every paint and every verdict needs it. */
    double mMedianConfidence = -1.0;

    int mHoveredBar = -1;

    bool mRollVisible = false;

    // Both halves of the same question as NoteOptionsView asked: has anything been transcribed,
    // and is it the same thing as last time we looked.
    bool mHasReading = false;
    std::uint32_t mLastNoteRevision = 0;

    // Laid out in resized() and drawn in paint(), so the two cannot drift apart.
    Rectangle<int> mReadoutBounds;
    Rectangle<int> mStripLabelBounds;
    Rectangle<int> mStripBounds;
    Rectangle<int> mRulerBounds;
    Rectangle<int> mVerdictBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptionSummary)
};

#endif // TranscriptionSummary_h
