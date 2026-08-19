//
// Created by Damien Ronssin on 11.03.23.
//

#include "VisualizationPanel.h"

#include <QuarryTooltips.h>

VisualizationPanel::VisualizationPanel(QuarryAudioProcessor* processor)
    : mProcessor(processor)
    , mCombinedAudioMidiRegion(processor, mKeyboard)
    , mMidiFileDrag(processor)
    , mSummary(*processor)
{
    mAudioMidiViewport.setViewedComponent(&mCombinedAudioMidiRegion);
    addAndMakeVisible(mAudioMidiViewport);
    mCombinedAudioMidiRegion.setViewportPtr(&mAudioMidiViewport);

    // Kept as a child so it lays out and answers geometry questions, never shown.
    addChildComponent(mKeyboard);

    mAudioMidiViewport.setScrollBarsShown(false, true, false, false);
    addChildComponent(mMidiFileDrag);

    addAndMakeVisible(mSummary);
    mSummary.onRollVisibilityToggled = [this](bool shouldShow) { _setRollVisible(shouldShow); };

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

    _setRollVisible(mShowRoll);
}

void VisualizationPanel::resized()
{
    // The summary is given exactly what it needs. Stretched to fill it was a readout with two
    // hundred pixels of nothing under the last sentence.
    const auto summaryHeight = jmin(getHeight() / 2,
                                    mShowRoll ? TranscriptionSummary::compactHeight()
                                              : TranscriptionSummary::naturalHeight());
    const auto scrollingHeight = jmax(120, getHeight() - summaryHeight - SUMMARY_GAP);

    // Whatever is left over goes to the waveform when the roll is away, which is most of the
    // time and is the point: it is the transport, and a take you can see the shape of is a
    // take you can scrub. With the roll up the waveform goes back to its old height and the
    // roll takes the remainder.
    mCombinedAudioMidiRegion.setAudioRegionHeight(
        mShowRoll ? CombinedAudioMidiRegion::DEFAULT_AUDIO_REGION_HEIGHT
                  : scrollingHeight - mCombinedAudioMidiRegion.mHeightBetweenAudioMidi);

    const auto rollY = mCombinedAudioMidiRegion.pianoRollY();

    // Against the roll's own height, not the panel's. PianoRoll asks the keyboard where every
    // pitch sits, so a keyboard laid out taller than the roll it answers for puts most of the
    // notes below the bottom edge - which is exactly what a shorter roll did to it.
    mKeyboard.setBounds(0, rollY, KEYBOARD_GEOMETRY_WIDTH, jmax(1, scrollingHeight - rollY));

    // The waveform and the roll share one horizontally scrolling region, because they share a
    // time axis and must not be able to disagree about where in the take you are looking.
    mAudioMidiViewport.setBounds(0, 0, getWidth(), scrollingHeight);

    mCombinedAudioMidiRegion.setBaseWidth(getWidth());
    mCombinedAudioMidiRegion.setBounds(0, 0, getWidth(), scrollingHeight);
    mCombinedAudioMidiRegion.changeListenerCallback(mProcessor->getSourceAudioManager()->getAudioThumbnail());

    // The strip between the waveform and wherever the roll would be: nobody's click target,
    // which is why the drag handle lives in it.
    mMidiFileDrag.setBounds(0, rollY - DRAG_BAND_HEIGHT, getWidth() - 6, DRAG_BAND_HEIGHT);

    mSummary.setBounds(0, getHeight() - summaryHeight, getWidth(), summaryHeight);

    mAudioGainSlider.setBounds(getWidth() - 205, 3, 200, 20);
    mMidiGainSlider.setBounds(getWidth() - 205, rollY + 3, 200, 20);

    mAudioRegionBounds = {0, 0, getWidth(), mCombinedAudioMidiRegion.audioRegionHeight()};
    mPianoRollBounds = {0, rollY, getWidth(), jmax(0, scrollingHeight - rollY)};
}

void VisualizationPanel::_setRollVisible(bool inShouldShow)
{
    mShowRoll = inShouldShow;

    mCombinedAudioMidiRegion.setPianoRollVisible(inShouldShow);
    mSummary.setRollVisible(inShouldShow);

    if (! inShouldShow)
        mMidiGainSlider.setVisible(false);

    resized();
}

void VisualizationPanel::clear()
{
    mCombinedAudioMidiRegion.setSize(getWidth(), mAudioMidiViewport.getHeight());
    mMidiFileDrag.setVisible(false);
    mSummary.clear();
}

void VisualizationPanel::repaintPianoRoll()
{
    mCombinedAudioMidiRegion.repaintPianoRoll();
}

void VisualizationPanel::setMidiFileDragComponentVisible()
{
    mMidiFileDrag.setVisible(true);
}

void VisualizationPanel::mouseEnter(const MouseEvent& event)
{
    Component::mouseEnter(event);

    if (mProcessor->getState() == PopulatedAudioAndMidiRegions) {
        if (event.originalComponent == mCombinedAudioMidiRegion.getAudioRegion()) {
            mAudioGainSlider.setVisible(true);
        } else if (mShowRoll && event.originalComponent == mCombinedAudioMidiRegion.getPianoRoll()) {
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
