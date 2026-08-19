//
// Created by Damien Ronssin on 11.03.23.
//

#ifndef VisualizationPanel_h
#define VisualizationPanel_h

#include <JuceHeader.h>

#include "CombinedAudioMidiRegion.h"
#include "Keyboard.h"
#include "MidiFileDrag.h"
#include "PluginProcessor.h"
#include "TranscriptionSummary.h"

/**
 * The right-hand two thirds of the Transcribe page: the waveform, the drag handle, and what
 * Quarry has to say about what it heard.
 *
 * The roll used to be the whole lower half and is now a thing you ask for. It answered no
 * question: the notes are not editable here and they are on their way to a host, so eighty-eight
 * lanes of rectangles were a picture of the export rather than a judgement of it.
 * TranscriptionSummary is what stands there instead, and the roll drops in underneath the
 * waveform when the summary's toggle asks for it.
 */
class VisualizationPanel : public Component
{
public:
    explicit VisualizationPanel(QuarryAudioProcessor* processor);

    ~VisualizationPanel() override = default;

    void resized() override;

    void clear();

    void repaintPianoRoll();

    void setMidiFileDragComponentVisible();

    void mouseEnter(const MouseEvent& event) override;

    void mouseExit(const MouseEvent& event) override;

    Viewport& getAudioMidiViewport();

    CombinedAudioMidiRegion& getCombinedAudioMidiRegion();

    // The keyboard is not drawn any more: it was the brightest thing on screen and
    // spent 50 px of width on a pitch ruler. It stays alive as a geometry source,
    // because PianoRoll asks it where every note and lane sits, so it keeps real
    // bounds while reserving no space in the layout.
    static constexpr int KEYBOARD_GEOMETRY_WIDTH = 50;

private:
    /** Shows or hides the roll, and gives its space to the summary either way. */
    void _setRollVisible(bool inShouldShow);

    // The band under the waveform, holding the midi file drag handle. It used to carry the
    // export tempo on its right end as well; the tempo is one of the things a take is
    // described by, so it moved into the summary with the rest of them.
    static constexpr int DRAG_BAND_HEIGHT = 14;

    static constexpr int SUMMARY_GAP = 10;

    QuarryAudioProcessor* mProcessor;
    Keyboard mKeyboard;
    Viewport mAudioMidiViewport;
    CombinedAudioMidiRegion mCombinedAudioMidiRegion;
    MidiFileDrag mMidiFileDrag;

    TranscriptionSummary mSummary;

    // Off by default: the summary is the answer, and the notes are the working. Not stored in
    // the value tree, because it is a thing you open to check something and close again.
    bool mShowRoll = false;

    Slider mAudioGainSlider;
    std::unique_ptr<SliderParameterAttachment> mAudioGainSliderAttachment;

    Slider mMidiGainSlider;
    std::unique_ptr<SliderParameterAttachment> mMidiGainSliderAttachment;

    Rectangle<int> mAudioRegionBounds;
    Rectangle<int> mPianoRollBounds;
};
#endif // VisualizationPanel_h
