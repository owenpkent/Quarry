//
// Created by Damien Ronssin on 12.03.23.
//

#ifndef NoteOptionsView_h
#define NoteOptionsView_h

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "NoteOptions.h"
#include "UIDefines.h"
#include "NoteUtils.h"
#include "MinMaxNoteSlider.h"
#include "QuarryTooltips.h"

class QuarryMainView;

class NoteOptionsView
    : public Component
    , AudioProcessorParameter::Listener

{
public:
    explicit NoteOptionsView(QuarryAudioProcessor& processor);

    ~NoteOptionsView() override;

    void resized() override;

    void paint(Graphics& g) override;

private:
    void parameterValueChanged(int parameterIndex, float newValue) override;

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    void _enableView(bool inEnable);

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

    bool mIsViewEnabled = false;
};

#endif // NoteOptionsView_h
