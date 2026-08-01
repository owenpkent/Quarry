//
// Created by Damien Ronssin on 11.03.23.
//

#include "VisualizationPanel.h"

#include <QuarryTooltips.h>

VisualizationPanel::VisualizationPanel(QuarryAudioProcessor* processor)
    : mProcessor(processor)
    , mCombinedAudioMidiRegion(processor, mKeyboard)
    , mMidiFileDrag(processor)
{
    mAudioMidiViewport.setViewedComponent(&mCombinedAudioMidiRegion);
    addAndMakeVisible(mAudioMidiViewport);
    mCombinedAudioMidiRegion.setViewportPtr(&mAudioMidiViewport);

    // Kept as a child so it lays out and answers geometry questions, never shown.
    addChildComponent(mKeyboard);

    mAudioMidiViewport.setScrollBarsShown(false, true, false, false);
    addChildComponent(mMidiFileDrag);

    auto tempo_str_validator = [](const String& tempo_str) {
        if (tempo_str.isEmpty()) {
            return false;
        }

        float tempo = tempo_str.getFloatValue();
        return tempo >= 20.0f && tempo <= 999.0f;
    };

    auto tempo_str_corrector = [](const String& tempo_str) {
        return tempo_str.isEmpty() ? String("120") : String(jlimit(20.0f, 999.0f, tempo_str.getFloatValue()));
    };

    mFileTempo = std::make_unique<NumericTextEditor<double>>(
        mProcessor, NnId::ExportTempoId, 6, 120.0, Justification::centred, tempo_str_validator, tempo_str_corrector);
    mFileTempo->setTooltip(QuarryTooltips::export_tempo);
    addChildComponent(*mFileTempo);

    mAudioGainSlider.setSliderStyle(Slider::SliderStyle::LinearHorizontal);
    mAudioGainSlider.setTextBoxStyle(Slider::TextEntryBoxPosition::TextBoxLeft, true, 40, 20);
    mAudioGainSlider.setTextValueSuffix(" dB");
    mAudioGainSlider.setColour(Slider::ColourIds::textBoxTextColourId, TEXT_MAIN);
    mAudioGainSlider.setColour(Slider::ColourIds::textBoxOutlineColourId, Colours::transparentWhite);
    // To also receive mouseExit callback from this slider
    mAudioGainSlider.addMouseListener(this, true);
    mAudioGainSlider.setTooltip(QuarryTooltips::source_audio_level);
    mAudioGainSliderAttachment = std::make_unique<SliderParameterAttachment>(
        *mProcessor->getParams()[ParameterHelpers::AudioPlayerGainId], mAudioGainSlider);

    addChildComponent(mAudioGainSlider);

    mMidiGainSlider.setSliderStyle(Slider::SliderStyle::LinearHorizontal);
    mMidiGainSlider.setTextBoxStyle(Slider::TextEntryBoxPosition::TextBoxLeft, true, 40, 20);
    mMidiGainSlider.setTextValueSuffix(" dB");
    mMidiGainSlider.setColour(Slider::ColourIds::textBoxTextColourId, TEXT_MAIN);
    mMidiGainSlider.setColour(Slider::ColourIds::textBoxOutlineColourId, Colours::transparentWhite);
    // To also receive mouseExit callback from this slider
    mMidiGainSlider.addMouseListener(this, true);
    mMidiGainSlider.setTooltip(QuarryTooltips::internal_synth_level);

    mMidiGainSliderAttachment = std::make_unique<SliderParameterAttachment>(
        *mProcessor->getParams()[ParameterHelpers::MidiPlayerGainId], mMidiGainSlider);

    addChildComponent(mMidiGainSlider);

    // Add this as mouse listener of audio region and pianoroll to control visibility of gain sliders
    mCombinedAudioMidiRegion.getAudioRegion()->addMouseListener(this, true);
    mCombinedAudioMidiRegion.getPianoRoll()->addMouseListener(this, true);
}

void VisualizationPanel::resized()
{
    mKeyboard.setBounds(0,
                        mCombinedAudioMidiRegion.mPianoRollY,
                        KEYBOARD_GEOMETRY_WIDTH,
                        getHeight() - mCombinedAudioMidiRegion.mPianoRollY);

    mAudioMidiViewport.setBounds(0, 0, getWidth(), getHeight());

    mCombinedAudioMidiRegion.setBaseWidth(getWidth());
    mCombinedAudioMidiRegion.setBounds(0, 0, getWidth(), getHeight());
    mCombinedAudioMidiRegion.changeListenerCallback(mProcessor->getSourceAudioManager()->getAudioThumbnail());

    // The export tempo used to sit in the keyboard gutter. With the gutter gone the
    // waveform runs to the left edge, and every pixel of it seeks the playhead, so
    // the tempo moves onto the strip between the waveform and the roll: the one band
    // in here that is nobody's click target. The drag handle gives up its right end.
    auto band =
        Rectangle<int>(0, mCombinedAudioMidiRegion.mPianoRollY - TEMPO_BAND_HEIGHT, getWidth(), TEMPO_BAND_HEIGHT);
    auto tempo_block = band.removeFromRight(TEMPO_BLOCK_WIDTH);

    mMidiFileDrag.setBounds(band.withTrimmedRight(6));

    mTempoBlockBounds = tempo_block;
    tempo_block.removeFromRight(8);
    mFileTempo->setBounds(tempo_block.removeFromRight(TEMPO_EDITOR_WIDTH));
    mTempoLabelBounds = tempo_block.withTrimmedLeft(8).withTrimmedRight(6);

    mAudioGainSlider.setBounds(getWidth() - 205, 3, 200, 20);
    mMidiGainSlider.setBounds(getWidth() - 205, mCombinedAudioMidiRegion.mPianoRollY + 3, 200, 20);

    mAudioRegionBounds = {0, 0, getWidth(), mCombinedAudioMidiRegion.mAudioRegionHeight};
    mPianoRollBounds = {
        0,
        mCombinedAudioMidiRegion.mAudioRegionHeight + mCombinedAudioMidiRegion.mHeightBetweenAudioMidi,
        getWidth(),
        getHeight() - (mCombinedAudioMidiRegion.mAudioRegionHeight + mCombinedAudioMidiRegion.mHeightBetweenAudioMidi)};
}

void VisualizationPanel::paint(Graphics& g)
{
    if (mMidiFileDrag.isVisible()) {
        g.setColour(PANEL_BG);
        g.fillRoundedRectangle(mTempoBlockBounds.toFloat(), 4);

        g.setColour(TEXT_MAIN);
        g.setFont(UIDefines::LABEL_FONT());
        g.drawFittedText("MIDI FILE TEMPO", mTempoLabelBounds, Justification::centredRight, 1);
    }
}

void VisualizationPanel::clear()
{
    mCombinedAudioMidiRegion.setSize(getWidth(), getHeight());
    mMidiFileDrag.setVisible(false);
    mFileTempo->setVisible(false);
}

void VisualizationPanel::repaintPianoRoll()
{
    mCombinedAudioMidiRegion.repaintPianoRoll();
}

void VisualizationPanel::setMidiFileDragComponentVisible()
{
    mMidiFileDrag.setVisible(true);
    mFileTempo->setVisible(true);
}

void VisualizationPanel::mouseEnter(const MouseEvent& event)
{
    Component::mouseEnter(event);

    if (mProcessor->getState() == PopulatedAudioAndMidiRegions) {
        if (event.originalComponent == mCombinedAudioMidiRegion.getAudioRegion()) {
            mAudioGainSlider.setVisible(true);
        } else if (event.originalComponent == mCombinedAudioMidiRegion.getPianoRoll()) {
            mMidiGainSlider.setVisible(true);
        }
    }
}

void VisualizationPanel::mouseExit(const MouseEvent& event)
{
    Component::mouseExit(event);

    if (mAudioGainSlider.isVisible()) {
        if (!mAudioRegionBounds.contains(getMouseXYRelative()))
            mAudioGainSlider.setVisible(false);
    }

    if (mMidiGainSlider.isVisible()) {
        if (!mPianoRollBounds.contains(getMouseXYRelative()))
            mMidiGainSlider.setVisible(false);
    }
}

Viewport& VisualizationPanel::getAudioMidiViewport()
{
    return mAudioMidiViewport;
}

CombinedAudioMidiRegion& VisualizationPanel::getCombinedAudioMidiRegion()
{
    return mCombinedAudioMidiRegion;
}
