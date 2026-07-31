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
    /** The stem shared by both files, without an extension. */
    String _nextBaseName() const;

    /** Where takes land. Falls back to Music/Quarry Samples on a first run. */
    File _folder() const;

    void _chooseFolder();

    void _save();

    void _updateEnablements();

    void _setStatus(const String& inText, bool inIsError);

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<TextButton> mFolderButton;
    std::unique_ptr<ToggleButton> mWavToggle;
    std::unique_ptr<ToggleButton> mMidiToggle;
    std::unique_ptr<TextButton> mSaveButton;
    std::unique_ptr<Label> mStatusLabel;

    std::unique_ptr<FileChooser> mFileChooser;

    MidiFileWriter mMidiFileWriter;

    // Cleared on the next state change so a result does not sit there forever.
    State mPreviousState = EmptyAudioAndMidiRegions;

    bool mShowingResult = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleBar)
};

#endif // SampleBar_h
