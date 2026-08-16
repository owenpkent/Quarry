//
// Created by Damien Ronssin on 06.03.23.
//

#ifndef PluginMainView_h
#define PluginMainView_h

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "Knob.h"
#include "NoteOptionsView.h"
#include "TimeQuantizeOptionsView.h"
#include "TranscriptionOptionsView.h"
#include "VisualizationPanel.h"
#include "AudioInputView.h"
#include "SampleBar.h"
#include <okstudio/Obsidian.h>
#include "NnId.h"
#include "UpdateCheck.h"

// Forward declared rather than included: its header reaches the Windows audio stack, and
// windows.h defines a Rectangle() that makes juce::Rectangle ambiguous in everything parsed
// after it. The include lives at the bottom of QuarryMainView.cpp instead.
#if JUCE_WINDOWS
class SamplePageView;
#endif

class QuarryMainView
    : public Component
    , public Timer
    , public ValueTree::Listener
{
public:
    explicit QuarryMainView(QuarryAudioProcessor& processor);

    ~QuarryMainView() override;

    void resized() override;

    void paint(Graphics& g) override;

    void timerCallback() override;

    void repaintPianoRoll();

    bool keyPressed(const KeyPress& key) override;

private:
    void updateEnablements();

    void valueTreePropertyChanged(ValueTree& treeWhosePropertyHasChanged, const Identifier& property) override;

    void _updateSettingsMenuTicks();

    void _updateTooltipVisibility();

    /** Swaps the whole content area between the two pages. Transcribe is everything that was
        here before; Sample is the capture page. The toolbar's transport belongs to
        Transcribe and goes with it, because none of it means anything on the other page. */
    void _showSamplePage(bool inShouldShow);

    QuarryAudioProcessor& mProcessor;

    State mPrevState = EmptyAudioAndMidiRegions;

    VisualizationPanel mVisualizationPanel;
    TranscriptionOptionsView mTranscriptionOptions;
    NoteOptionsView mNoteOptions;
    TimeQuantizeOptionsView mQuantizePanel;

    std::unique_ptr<DrawableButton> mMuteButton;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> mMuteButtonAttachment;

    std::unique_ptr<DrawableButton> mRecordButton;
    std::unique_ptr<DrawableButton> mClearButton;

    std::unique_ptr<AudioInputView> mAudioInputView;

    std::unique_ptr<DrawableButton> mBackButton;
    std::unique_ptr<DrawableButton> mPlayPauseButton;
    std::unique_ptr<DrawableButton> mCenterButton;
    std::unique_ptr<DrawableButton> mSettingsButton;

    std::unique_ptr<SampleBar> mSampleBar;

#if JUCE_WINDOWS
    std::unique_ptr<TextButton> mTranscribeTab;
    std::unique_ptr<TextButton> mSampleTab;
    std::unique_ptr<SamplePageView> mSamplePage;
#endif

    std::unique_ptr<TooltipWindow> mTooltipWindow;

    class PopupMenuLookAndFeel : public LookAndFeel_V4
    {
        Font getPopupMenuFont() override { return UIDefines::LABEL_FONT(); }
    };

    std::unique_ptr<PopupMenuLookAndFeel> mPopupMenuLookAndFeel;
    // Define the settings menu after the look and feel, so it is destroyed first
    std::unique_ptr<PopupMenu> mSettingsMenu;

    std::vector<std::pair<int, std::function<bool()>>> mSettingsMenuItemsShouldBeTicked;

    std::unique_ptr<Knob> mMinNoteSlider;
    std::unique_ptr<Knob> mMaxNoteSlider;

    std::unique_ptr<ComboBox> mKey; // C, C#, D, D# ...
    std::unique_ptr<ComboBox> mMode; // Major, Minor, Chromatic

    Image mBackgroundImage;

    int mNumCallbacksStuckInProcessingState = 0;

    std::unique_ptr<UpdateCheck> mUpdateCheck;
};

#endif // PluginMainView_h
