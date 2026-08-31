//
// Created by Damien Ronssin on 12.03.23.
//

#ifndef NoteOptionsView_h
#define NoteOptionsView_h

#include "LeftColumnLayout.h"

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
    , public AsyncUpdater
{
public:
    explicit NoteOptionsView(QuarryAudioProcessor& processor);

    ~NoteOptionsView() override;

    void resized() override;

    void paint(Graphics& g) override;

    /**
     * The panel's height for the state it is in: full when scale quantization is on, its own
     * label row when it is off.
     *
     * Off is the default, and two sections defaulting to off were holding 254 px of the most
     * prominent column in the window to show controls that do nothing until someone turns them
     * on. Collapsing gives that space to the choice the page actually turns on, and makes the
     * difference between a section that is acting on the take and one that is not impossible
     * to misread.
     */
    int preferredHeight() const;

    /** Called when preferredHeight() has changed and the left column needs re-stacking. */
    std::function<void()> onPreferredHeightChanged;

private:
    void parameterValueChanged(int parameterIndex, float newValue) override;

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    void handleAsyncUpdate() override;

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

    // What parameterValueChanged saw, for handleAsyncUpdate to act on once it is on the message
    // thread. An atomic rather than a captured lambda argument because the write happens on the
    // audio thread, where allocating one is not allowed.
    std::atomic<bool> mPendingEnable {false};
};

#endif // NoteOptionsView_h
