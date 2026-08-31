//
// Created by Damien Ronssin on 12.03.23.
//

#ifndef RhythmOptionsView_h
#define RhythmOptionsView_h

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "UIDefines.h"
#include "TimeQuantizeUtils.h"
#include "QuantizeForceSlider.h"
#include "NumericTextEditor.h"
#include "QuarryTooltips.h"
#include "LeftColumnLayout.h"

class QuarryMainView;

class TimeQuantizeOptionsView
    : public Component
    , public AudioProcessorParameter::Listener
    , public AsyncUpdater
{
public:
    explicit TimeQuantizeOptionsView(QuarryAudioProcessor& processor);

    ~TimeQuantizeOptionsView() override;

    void resized() override;

    void paint(Graphics& g) override;

    /**
     * The panel's height for the state it is in: full when time quantization is on, its own
     * label row when it is off. See NoteOptionsView::preferredHeight for why a section that
     * defaults to off does not get to keep its space.
     */
    int preferredHeight() const;

    /** Called when preferredHeight() has changed and the left column needs re-stacking. */
    std::function<void()> onPreferredHeightChanged;

private:
    void parameterValueChanged(int parameterIndex, float newValue) override;

    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

    void handleAsyncUpdate() override;

    void _setViewEnabled(bool inEnable);

    void _setupTempoEditor();

    void _setupTSEditors();

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<TextButton> mEnableButton;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> mEnableAttachment;

    std::unique_ptr<ComboBox> mTimeDivisionDropdown;
    std::unique_ptr<ComboBoxParameterAttachment> mTimeDivisionAttachment;

    std::unique_ptr<QuantizeForceSlider> mQuantizationForceSlider;

    std::unique_ptr<NumericTextEditor<double>> mTempoEditor;

    std::unique_ptr<NumericTextEditor<int>> mTimeSignatureNumEditor;
    std::unique_ptr<NumericTextEditor<int>> mTimeSignatureDenomEditor;

    bool mIsViewEnabled = false;

    // Written on whichever thread moved the parameter, read on the message thread by
    // handleAsyncUpdate. Same reason as NoteOptionsView's.
    std::atomic<bool> mPendingEnable {false};
};

#endif // RhythmOptionsView_h
