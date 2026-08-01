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
#include "VisualizationPanel.h"
#include "NumericTextEditor.h"

class VisualizationPanel : public Component
{
public:
    explicit VisualizationPanel(QuarryAudioProcessor* processor);

    ~VisualizationPanel() override = default;

    void resized() override;

    void paint(Graphics& g) override;

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
    // The band between the waveform and the roll: the midi file drag handle on the
    // left, the export tempo and its caption on the right.
    static constexpr int TEMPO_BAND_HEIGHT = 14;
    static constexpr int TEMPO_BLOCK_WIDTH = 170;
    static constexpr int TEMPO_EDITOR_WIDTH = 44;

    QuarryAudioProcessor* mProcessor;
    Keyboard mKeyboard;
    Viewport mAudioMidiViewport;
    CombinedAudioMidiRegion mCombinedAudioMidiRegion;
    MidiFileDrag mMidiFileDrag;

    Slider mAudioGainSlider;
    std::unique_ptr<SliderParameterAttachment> mAudioGainSliderAttachment;

    Slider mMidiGainSlider;
    std::unique_ptr<SliderParameterAttachment> mMidiGainSliderAttachment;

    Rectangle<int> mAudioRegionBounds;
    Rectangle<int> mPianoRollBounds;
    Rectangle<int> mTempoBlockBounds;
    Rectangle<int> mTempoLabelBounds;

    std::unique_ptr<NumericTextEditor<double>> mFileTempo;
};
#endif // VisualizationPanel_h
