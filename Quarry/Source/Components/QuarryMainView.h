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
#include "ModelOptionsView.h"
#include "VisualizationPanel.h"
#include "AudioInputView.h"
#include "SampleBar.h"
#include "ActivityDrawer.h"
#include "ProgressStrip.h"
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

    /** The editor is not a parent yet when this is constructed, so the window cannot be sized
        to the page until it is. */
    void parentHierarchyChanged() override;

    void paint(Graphics& g) override;

    void timerCallback() override;

    void repaintPianoRoll();

    bool keyPressed(const KeyPress& key) override;

private:
    void updateEnablements();

    void valueTreePropertyChanged(ValueTree& treeWhosePropertyHasChanged, const Identifier& property) override;

    void _updateSettingsMenuTicks();

    void _updateTooltipVisibility();

    /** Swaps the whole content area between the two pages. Sample is where the window opens:
        the app captures audio first, and transcribing is something you then do to a capture.
        The toolbar's transport belongs to Transcribe and goes with it, because none of it
        means anything on the other page. */
    void _showSamplePage(bool inShouldShow);

    /** Opens the activity drawer on Transcribe, closes it anywhere. The key and the footer button
        both come here so the page rule lives in one place. */
    void _toggleActivityDrawer();

    /** Sizes the window to whatever is on screen. Only the Sample page with its captures hidden
        asks for anything but the full width; Transcribe's layout is absolute and assumes it. */
    void _applyWindowSize();

    /**
     * Stacks the three left-hand sections top down, asking each how tall it wants to be.
     *
     * Absolute coordinates do not survive this column any more. Two of the three sections
     * collapse to their label row when their toggle is off, which is the state they are both in
     * by default, and the one above them grows when ADVANCED opens.
     */
    void _layoutLeftColumn();

    QuarryAudioProcessor& mProcessor;

    State mPrevState = EmptyAudioAndMidiRegions;

    VisualizationPanel mVisualizationPanel;
    ModelOptionsView mModelOptions;
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

    // Added last (see the constructor) so it paints over the column and the footer rather than
    // under them. Hidden until the backtick key or the footer button opens it; see
    // ActivityDrawerLayout for why it is an overlay and not a fourth section.
    std::unique_ptr<ActivityDrawer> mActivityDrawer;

    /** The header's progress bar, caption and Cancel, above the transport. Hides itself when no
        job is running; see ProgressStripLayout for where it sits and why. */
    std::unique_ptr<ProgressStrip> mProgressStrip;

#if JUCE_WINDOWS
    /** The way back out of Transcribe, and the only navigation the toolbar needs now that the
        two pages are not peers. Shown on Transcribe, absent on the page it returns to. */
    std::unique_ptr<TextButton> mBackToSamplesButton;
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
