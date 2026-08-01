//
// Created by Damien Ronssin on 12.03.23.
//

#ifndef NoteOptionsView_h
#define NoteOptionsView_h

#include <cstdint>

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "NoteOptions.h"
#include "UIDefines.h"
#include "NoteUtils.h"
#include "MinMaxNoteSlider.h"
#include "QuarryTooltips.h"
#include "KeyEstimate.h"

class QuarryMainView;

class NoteOptionsView
    : public Component
    , public Timer
    , AudioProcessorParameter::Listener

{
public:
    explicit NoteOptionsView(QuarryAudioProcessor& processor);

    ~NoteOptionsView() override;

    void resized() override;

    void paint(Graphics& g) override;

    void timerCallback() override;

private:
    void parameterValueChanged(int parameterIndex, float newValue) override;

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    void _enableView(bool inEnable);

    /** Recompute only when the transcription actually changed. */
    void _refreshDetectedKey();

    /** Back to the state before anything has been transcribed. */
    void _clearDetectedKey();

    /** Push the detected key into the snap controls. */
    void _adoptDetectedKey();

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<TextButton> mEnableButton;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> mEnableAttachment;

    std::unique_ptr<MinMaxNoteSlider> mMinMaxNoteSlider;

    std::unique_ptr<ComboBox> mRootNoteDropdown;
    std::unique_ptr<ComboBoxParameterAttachment> mKeyAttachment;

    std::unique_ptr<ComboBox> mKeyType;
    std::unique_ptr<ComboBoxParameterAttachment> mKeyTypeAttachment;

    std::unique_ptr<ComboBox> mSnapMode;
    std::unique_ptr<ComboBoxParameterAttachment> mSnapModeAttachment;

    std::unique_ptr<Label> mDetectedLabel;
    std::unique_ptr<TextButton> mUseKeyButton;

    KeyEstimate mDetected;

    // Whether the readout is showing a judgement of a transcription at all, as opposed to the
    // nothing-yet state. Without it the revision below cannot tell a first look from a repeat.
    bool mHasReading = false;
    std::uint32_t mLastNoteRevision = 0;

    bool mIsViewEnabled = false;
};

#endif // NoteOptionsView_h
