//
// Created by Damien Ronssin on 11.03.23.
//

#ifndef CombinedAudioMidiRegion_h
#define CombinedAudioMidiRegion_h

#include <JuceHeader.h>

#include "AudioRegion.h"
#include "Keyboard.h"
#include "PianoRoll.h"
#include "PluginProcessor.h"

class CombinedAudioMidiRegion
    : public Component
    , public FileDragAndDropTarget
    , public ChangeListener
    , public ValueTree::Listener
{
public:
    CombinedAudioMidiRegion(QuarryAudioProcessor* processor, Keyboard& keyboard);

    ~CombinedAudioMidiRegion() override;

    void setViewportPtr(juce::Viewport* inViewportPtr);

    void resized() override;

    void paint(Graphics& g) override;

    bool isInterestedInFileDrag(const StringArray& files) override;

    void filesDropped(const StringArray& files, int x, int y) override;

    void fileDragEnter(const StringArray& files, int x, int y) override;

    void fileDragExit(const StringArray& files) override;

    void setBaseWidth(int inWidth);

    /** Shows or hides the roll under the waveform. Hidden, it takes no height and no clicks:
        the region is sized to the waveform alone and the roll is not a hit target that is
        merely invisible. */
    void setPianoRollVisible(bool inShouldShow);

    void repaintPianoRoll();

    void resizeAccordingToNumSamplesAvailable();

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    void setCenterView(bool inShouldCenterView);

    void mouseWheelMove(const MouseEvent& event, const MouseWheelDetails& wheel) override;

    void mouseMagnify(const MouseEvent& event, float scaleFactor) override;

    AudioRegion* getAudioRegion();

    PianoRoll* getPianoRoll();

    /** How tall the waveform is. Was a constant at 85 px, back when the roll took the rest of
        the panel whether or not it was earning it. With the roll away there is a great deal of
        height going spare and the waveform is the thing that wants it: it is the transport,
        every pixel of it seeks, and the shape of a take is what you scrub against. */
    void setAudioRegionHeight(int inHeight);

    int audioRegionHeight() const { return mAudioRegionHeight; }

    int pianoRollY() const { return mAudioRegionHeight + mHeightBetweenAudioMidi; }

    const double mBaseNumPixelsPerSecond = 100.0;

    static constexpr int DEFAULT_AUDIO_REGION_HEIGHT = 85;

    const int mHeightBetweenAudioMidi = 23;

private:
    void _onVBlankCallback();

    void _centerViewOnPlayhead();

    bool _isFileTypeSupported(const String& filename) const;

    void _setZoomLevel(double inZoomLevel);

    void valueTreePropertyChanged(ValueTree& treeWhosePropertyHasChanged, const Identifier& property) override;

    QuarryAudioProcessor* mProcessor;

    juce::Viewport* mViewportPtr = nullptr;
    juce::VBlankAttachment mVBlankAttachment;

    const StringArray mSupportedAudioFileExtensions;

    bool mShouldCenterView = false;

    int mBaseWidth = 0;

    const double mMaxZoomLevel = 5.0;
    const double mMinZoomLevel = 0.1;
    double mZoomLevel = 1.0;

    int mAudioRegionHeight = DEFAULT_AUDIO_REGION_HEIGHT;

    AudioRegion mAudioRegion;
    PianoRoll mPianoRoll;
};

#endif // CombinedAudioMidiRegion_h
