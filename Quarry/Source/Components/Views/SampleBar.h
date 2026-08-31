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

private:
    /** The stem shared by both files, without an extension. Walks inFolder, so the timer
        reads mNextBaseName instead of calling this.
    */
    String _nextBaseName(const File& inFolder) const;

    void _refreshNextBaseName(const File& inFolder);

    /** Where takes land. Falls back to Music/Quarry Samples on a first run. */
    File _folder() const;

    void _chooseFolder();

    /** Gathers what the write needs, then hands it to a background thread. */
    void _save();

    /** Back on the message thread once the writing is done. */
    void _finishSave(const StringArray& inWritten, const StringArray& inProblems);

    void _updateEnablements();

    void _setStatus(const String& inText, bool inIsError);

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<TextButton> mFolderButton;

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
