//
// Keeping a take: a folder you pick once, two format toggles, and one button.
//

#ifndef SampleBar_h
#define SampleBar_h

#include "JuceHeader.h"

#include "MidiFileWriter.h"
#include "PluginProcessor.h"
#include "UIDefines.h"

/**
    The bar along the bottom of the window that writes a finished take to disk.

    Deliberately not a menu: the destination, the formats and the name of the
    next take are all on screen, so saving is one click and never a dialog. The
    destination and formats are project state, so a session reopened another day
    writes where it wrote before.
*/
class SampleBar
    : public Component
    , public Timer
{
public:
    explicit SampleBar(QuarryAudioProcessor& inProcessor);

    ~SampleBar() override;

    void paint(Graphics& g) override;

    void resized() override;

    void timerCallback() override;

    /** Toggles the activity drawer. Set by whoever owns both this bar and the drawer; the bar
        only knows it has a button to click, not what the drawer is. */
    std::function<void()> onToggleActivity;

private:
    /** The stem shared by both files, without an extension. Walks inFolder, so the timer
        reads mNextBaseName instead of calling this.
    */
    String _nextBaseName(const File& inFolder) const;

    void _refreshNextBaseName(const File& inFolder);

    /** Where takes land. Falls back to Music/Quarry Samples on a first run. */
    File _folder() const;

    /** What the folder button shows: the last two components of the path, which fit a button in
        a fixed-width bar where the whole path does not. The tooltip keeps the whole thing. */
    String _folderLabel(const File& inFolder) const;

    void _chooseFolder();

    /** Shows the save folder in the desktop's own file manager, creating it first if a first
        run has named it but nothing has written to it yet. */
    void _openFolder();

    /** Gathers what the write needs, then hands it to a background thread. */
    void _save();

    /** Back on the message thread once the writing is done. */
    void _finishSave(const StringArray& inWritten, const StringArray& inProblems);

    void _updateEnablements();

    void _setStatus(const String& inText, bool inIsError);

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<TextButton> mFolderButton;

    // Beside the path rather than replacing the click on it: the path button changes where takes
    // go, which is the rarer thing to want and the one you would not want to do by accident, and
    // "open the folder I have been saving to" had no control at all. Two narrow buttons that read
    // as one, rather than one button whose two jobs a person has to guess between.
    std::unique_ptr<DrawableButton> mOpenFolderButton;

    // Beside the folder pair for the same reason Open sits beside the path: a dev-facing panel
    // with its own toggle earns its own icon rather than a menu entry nobody working on the
    // product would think to open. The bar only fires onToggleActivity -- it does not know the
    // drawer exists, the same way it does not know what "open" shows in the file manager.
    std::unique_ptr<DrawableButton> mActivityToggleButton;

    // Pitch bend is an export setting, not a transcription one: it changes the MIDI written
    // from a take and nothing about what the model heard from it. docs/UI.md draws that line
    // itself -- a control that acts on the result belongs in the footer -- and it spent years
    // in the left column's TRANSCRIPTION panel anyway, beside three decoder knobs it has
    // nothing to do with.
    std::unique_ptr<ComboBox> mPitchBend;
    std::unique_ptr<ComboBoxParameterAttachment> mPitchBendAttachment;

    std::unique_ptr<ToggleButton> mWavToggle;
    std::unique_ptr<ToggleButton> mMidiToggle;
    std::unique_ptr<TextButton> mSaveButton;
    std::unique_ptr<Label> mStatusLabel;

    std::unique_ptr<FileChooser> mFileChooser;

    // Cleared on the next state change so a result does not sit there forever.
    State mPreviousState = EmptyAudioAndMidiRegions;

    bool mShowingResult = false;

    // Decoding an hour of audio is not something to do on the message thread, so the write
    // runs elsewhere and this keeps a second click from starting a race with the first.
    bool mSaveInFlight = false;

    // Working the next name out walks the save folder, which can be a network share, so it
    // is remembered until a save, a new take or a different folder can have changed it.
    String mNextBaseName;
    String mNextBaseNameFolder;
    bool mNextBaseNameStale = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleBar)
};

#endif // SampleBar_h
