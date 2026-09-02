//
// Keeping a take: a folder you pick once, two format toggles, and one button.
//

#include "SampleBar.h"

#include "QuarryLookAndFeel.h"

#include "AudioUtils.h"
#include "QuarryTooltips.h"
#include "SampleBarLayout.h"
#include "TakeNaming.h"

#include <okstudio/Obsidian.h>

namespace
{
constexpr int kMaxTakeNumber = 9999;

/** Opens a wav writer on inDestination, leaving nothing behind if it cannot. */
std::unique_ptr<AudioFormatWriter>
    makeWavWriter(const File& inDestination, double inSampleRate, unsigned int inNumChannels, int inBitDepth)
{
    // A FileOutputStream opens an existing file and seeks to the end, so without this a
    // second wav would be glued onto the first rather than replacing it.
    inDestination.deleteFile();

    auto stream = std::make_unique<FileOutputStream>(inDestination);

    if (stream->failedToOpen())
        return nullptr;

    WavAudioFormat format;
    StringPairArray meta_data_values;

    std::unique_ptr<AudioFormatWriter> writer(
        format.createWriterFor(stream.get(), inSampleRate, inNumChannels, inBitDepth, meta_data_values, 0));

    if (writer == nullptr) {
        stream.reset();
        inDestination.deleteFile();
        return nullptr;
    }

    // createWriterFor only takes the stream on when it succeeds.
    stream.release();

    return writer;
}
} // namespace

SampleBar::SampleBar(QuarryAudioProcessor& inProcessor)
    : mProcessor(inProcessor)
{
    mFolderButton = std::make_unique<TextButton>("FolderButton");
    mFolderButton->setTooltip("Choose where saved takes go.");
    mFolderButton->onClick = [this]() { _chooseFolder(); };
    addAndMakeVisible(*mFolderButton);

    mOpenFolderButton =
        std::make_unique<DrawableButton>("OpenFolderButton", DrawableButton::ButtonStyle::ImageFitted);
    mOpenFolderButton->setColour(DrawableButton::ColourIds::backgroundColourId, Colours::transparentBlack);
    mOpenFolderButton->setColour(DrawableButton::ColourIds::backgroundOnColourId, Colours::transparentBlack);

    auto open_folder_icon = quarry::lnf::icon(okstudio::icons::folderOpen, TEXT_DIM);
    mOpenFolderButton->setImages(open_folder_icon.get());

    // A glyph has no text for a screen reader to read, so the name has to be written. The path
    // button beside it already says where; this one only has to say what it does with it, and
    // "Open" on its own -- read out of a bar that also opens a chooser and writes a file -- could
    // be any of three things.
    mOpenFolderButton->setTitle("Open the save folder");
    mOpenFolderButton->setTooltip("Show the save folder in the file manager.");
    mOpenFolderButton->onClick = [this]() { _openFolder(); };
    addAndMakeVisible(*mOpenFolderButton);

    mActivityToggleButton =
        std::make_unique<DrawableButton>("ActivityToggleButton", DrawableButton::ButtonStyle::ImageFitted);
    mActivityToggleButton->setColour(DrawableButton::ColourIds::backgroundColourId, Colours::transparentBlack);
    mActivityToggleButton->setColour(DrawableButton::ColourIds::backgroundOnColourId, Colours::transparentBlack);

    // chevronDown rather than anything closer to a terminal or a log: the icon set is Lucide's
    // interface glyphs, none of which draw a console, and this is the one already named for
    // revealing something that sits below what is on screen, which is what the drawer does.
    auto activity_icon = quarry::lnf::icon(okstudio::icons::chevronDown, TEXT_DIM);
    mActivityToggleButton->setImages(activity_icon.get());
    mActivityToggleButton->setTitle("Toggle the activity log");
    mActivityToggleButton->setTooltip("Show or hide the activity log.");
    mActivityToggleButton->onClick = [this]() {
        if (onToggleActivity != nullptr)
            onToggleActivity();
    };
    addAndMakeVisible(*mActivityToggleButton);

    // Bound to the tree rather than read from it once, so a session loaded with the editor
    // open moves the boxes instead of leaving them showing the previous session's answer.
    // A tree-driven change reaches onStateChange but never onClick.
    mWavToggle = std::make_unique<ToggleButton>("Wav");
    mWavToggle->getToggleStateValue().referTo(
        mProcessor.getValueTree().getPropertyAsValue(NnId::SampleWriteWavId, nullptr));
    mWavToggle->setTooltip("Write the recorded audio.");
    addAndMakeVisible(*mWavToggle);

    mMidiToggle = std::make_unique<ToggleButton>("Midi");
    mMidiToggle->getToggleStateValue().referTo(
        mProcessor.getValueTree().getPropertyAsValue(NnId::SampleWriteMidiId, nullptr));
    mMidiToggle->setTooltip("Write the transcription.");
    addAndMakeVisible(*mMidiToggle);

    mPitchBend = std::make_unique<ComboBox>("PitchBend");
    mPitchBend->setEditableText(false);
    mPitchBend->setJustificationType(Justification::centredLeft);
    // The parameter's own choice names, not shortened ones. They were shortened while a painted
    // "PITCH BEND" caption sat beside the picker saying what it was; the caption cost 80 px of a
    // bar with none to spare (see SampleBarLayout), and these two words say it themselves.
    mPitchBend->addItemList({"No Pitch Bend", "Single Pitch Bend"}, 1);
    mPitchBend->setTooltip(QuarryTooltips::to_pitch_bend);
    mPitchBend->setTitle("Pitch bend mode");
    mPitchBendAttachment = std::make_unique<ComboBoxParameterAttachment>(
        *mProcessor.getParams()[ParameterHelpers::PitchBendModeId], *mPitchBend);
    addAndMakeVisible(*mPitchBend);

    mSaveButton = std::make_unique<TextButton>("Save");
    // The one action this bar exists for, and the only primary button on the page.
    // The role carries the accent fill and the dark-on-accent text; setting the colour
    // by hand here is what produced a Save that looked like "show notes".
    quarry::lnf::setRole(*mSaveButton, quarry::lnf::Role::primary);
    mSaveButton->setTitle("Save");
    mSaveButton->setTooltip("Write this take to the folder on the left.");
    mSaveButton->onClick = [this]() { _save(); };
    addAndMakeVisible(*mSaveButton);

    mStatusLabel = std::make_unique<Label>("SampleStatus");
    mStatusLabel->setJustificationType(Justification::centredRight);
    mStatusLabel->setColour(Label::textColourId, TEXT_DIM);
    mStatusLabel->setInterceptsMouseClicks(false, false);
    addAndMakeVisible(*mStatusLabel);

    // Hooked up last because _updateEnablements reads every child above.
    mWavToggle->onStateChange = [this]() { _updateEnablements(); };
    mMidiToggle->onStateChange = [this]() { _updateEnablements(); };

    _updateEnablements();
    startTimerHz(4);
}

SampleBar::~SampleBar()
{
    stopTimer();
}

String SampleBar::_folderLabel(const File& inFolder) const
{
    const auto name = inFolder.getFileName();
    const auto parent = inFolder.getParentDirectory().getFileName();

    // A drive root has no parent name to prefix with, and neither has a folder sitting directly
    // in one, so both fall back to the whole path -- which is short in exactly those cases.
    if (name.isEmpty() || parent.isEmpty())
        return inFolder.getFullPathName();

    return parent + File::getSeparatorString() + name;
}

File SampleBar::_folder() const
{
    return TakeNaming::folder(mProcessor.getValueTree());
}

String SampleBar::_nextBaseName(const File& inFolder) const
{
    // A dropped file keeps its own name, so a saved take is recognisable next to
    // the thing it came from. A recording gets the next free take number.
    const auto dropped = mProcessor.getSourceAudioManager()->getDroppedFilename();

    if (dropped.isNotEmpty())
        return File::createLegalFileName(dropped);

    const String prefix = "quarry-take-";
    int highest = 0;

    // One pass over the folder rather than a stat per candidate: the folder can be a
    // network share and this runs to draw a hint, not to save anything.
    if (inFolder.isDirectory()) {
        for (const auto& entry: RangedDirectoryIterator(inFolder, false, prefix + "*", File::findFiles)) {
            const auto suffix = entry.getFile().getFileNameWithoutExtension().substring(prefix.length());
            highest = jmax(highest, suffix.getIntValue());
        }
    }

    const int next = highest + 1;

    if (next > kMaxTakeNumber)
        return "quarry-take";

    return prefix + String(next).paddedLeft('0', 3);
}

void SampleBar::_refreshNextBaseName(const File& inFolder)
{
    mNextBaseName = _nextBaseName(inFolder);
    mNextBaseNameFolder = inFolder.getFullPathName();
    mNextBaseNameStale = false;
}

void SampleBar::_openFolder()
{
    const auto folder = _folder();

    // Created rather than reported as missing. TakeNaming::folder answers with Music/Quarry
    // Samples on a first run, and that directory does not exist until the first save writes to
    // it -- so on a fresh install the folder the bar has been naming all along would be the one
    // thing this button could not show anyone.
    if (!folder.isDirectory()) {
        const auto created = folder.createDirectory();

        if (created.failed()) {
            _setStatus("Could not open " + folder.getFullPathName() + ": " + created.getErrorMessage(), true);
            return;
        }
    }

    // Selects the folder in the file manager rather than opening it, which is what JUCE's own
    // name for this says and what every platform's "show in" idiom does. Either is fine here;
    // what matters is that it is the desktop's own window and not one of ours.
    folder.revealToUser();
}

void SampleBar::_chooseFolder()
{
    mFileChooser = std::make_unique<FileChooser>("Where should saved takes go?", _folder());

    const auto flags = FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories;

    // The dialog outlives nothing here by design, but closing the editor while it is open
    // would otherwise call back into a bar that has gone.
    Component::SafePointer<SampleBar> safe_this(this);

    mFileChooser->launchAsync(flags, [safe_this](const FileChooser& inChooser) {
        auto* self = safe_this.getComponent();

        if (self == nullptr)
            return;

        const auto chosen = inChooser.getResult();

        if (chosen == File())
            return;

        self->mProcessor.getValueTree().setProperty(NnId::SampleFolderId, chosen.getFullPathName(), nullptr);
        self->mShowingResult = false;
        self->_updateEnablements();
    });
}

namespace
{
Result writeWav(const File& inSource, const File& inDestination)
{
    // A recording is already a wav written by a wav writer, so copying it is exact and free.
    if (inSource.hasFileExtension("wav")) {
        if (!inSource.copyFileTo(inDestination))
            return Result::fail("Could not write " + inDestination.getFileName());

        return Result::ok();
    }

    const auto write_failed = [&inDestination]() {
        inDestination.deleteFile();
        return Result::fail("Could not write " + inDestination.getFileName());
    };

    // A dropped file is whatever the user dragged in. Its bytes under a .wav name would be
    // a file nothing can open, Quarry included, so it is decoded and re-encoded instead.
    AudioFormatManager format_manager;
    format_manager.registerBasicFormats();

    std::unique_ptr<AudioFormatReader> reader(format_manager.createReaderFor(inSource));

    if (reader != nullptr) {
        // Snapped rather than clamped: a wav writer takes 8, 16, 24 or 32 and refuses
        // everything else, so the 20 bits a flac can report would write nothing at all.
        const int bit_depth = (reader->usesFloatingPointData || reader->bitsPerSample > 16) ? 24 : 16;

        auto writer = makeWavWriter(inDestination, reader->sampleRate, reader->numChannels, bit_depth);

        if (writer == nullptr)
            return write_failed();

        // Block by block: a dropped file can be an hour long.
        const bool ok = writer->writeFromAudioReader(*reader, 0, reader->lengthInSamples);
        writer.reset();

        return ok ? Result::ok() : write_failed();
    }

    // Some formats are decoded by the project's own loader rather than by JUCE, so there is
    // no reader to stream from and the whole thing has to come into memory.
    AudioBuffer<float> decoded;
    double sample_rate = 0.0;

    if (!AudioUtils::loadAudioFile(inSource, decoded, sample_rate) || decoded.getNumSamples() == 0
        || sample_rate <= 0.0) {
        return Result::fail("Could not read " + inSource.getFileName());
    }

    auto writer = makeWavWriter(inDestination, sample_rate, static_cast<unsigned int>(decoded.getNumChannels()), 24);

    if (writer == nullptr)
        return write_failed();

    const bool ok = writer->writeFromAudioSampleBuffer(decoded, 0, decoded.getNumSamples());
    writer.reset();

    return ok ? Result::ok() : write_failed();
}

/** Everything the write needs, copied off the message thread before it starts. */
struct SaveRequest {
    File folder;
    String stem;
    bool wantWav = false;
    bool wantMidi = false;
    File source;
    std::vector<Notes::Event> notes;
    std::vector<SidecarPedalEvent> pedalEvents;
    TimeQuantizeOptions::TimeQuantizeInfo quantizeInfo;
    double exportBpm = 120.0;
    PitchBendModes pitchBendMode = NoPitchBend;
};

/** Runs on a background thread, so it touches nothing but its own copy of the request. */
void runSave(const SaveRequest& inRequest, StringArray& outWritten, StringArray& outProblems)
{
    if (inRequest.wantWav) {
        if (!inRequest.source.existsAsFile()) {
            outProblems.add("The recorded audio is no longer on disk");
        } else {
            const auto destination = inRequest.folder.getChildFile(inRequest.stem + ".wav");
            const auto result = writeWav(inRequest.source, destination);

            if (result.wasOk())
                outWritten.add(destination.getFileName());
            else
                outProblems.add(result.getErrorMessage());
        }
    }

    if (inRequest.wantMidi) {
        const auto destination = inRequest.folder.getChildFile(inRequest.stem + ".mid");

        const MidiFileWriter writer;
        const bool ok = writer.writeMidiFile(inRequest.notes,
                                             destination,
                                             inRequest.quantizeInfo,
                                             inRequest.exportBpm,
                                             inRequest.pitchBendMode,
                                             inRequest.pedalEvents);

        if (ok)
            outWritten.add(destination.getFileName());
        else
            outProblems.add("Could not write " + destination.getFileName());
    }
}
} // namespace

void SampleBar::_save()
{
    if (mSaveInFlight)
        return;

    const auto folder = _folder();

    if (!folder.isDirectory()) {
        const auto created = folder.createDirectory();

        if (created.failed()) {
            _setStatus("Could not create " + folder.getFullPathName(), true);
            return;
        }
    }

    SaveRequest request;
    request.folder = folder;
    request.wantWav = mWavToggle->getToggleState();
    request.wantMidi = mMidiToggle->getToggleState();

    StringArray extensions;

    if (request.wantWav)
        extensions.add(".wav");

    if (request.wantMidi)
        extensions.add(".mid");

    // One stem cleared against every extension at once. Resolving them separately is how a take
    // ends up as loop(2).wav beside loop.mid, which is not the pair this feature exists to make.
    request.stem = TakeNaming::freeStem(folder, _nextBaseName(folder), extensions);

    if (request.stem.isEmpty()) {
        _setStatus("No free name left in " + folder.getFileName(), true);
        return;
    }

    // Read here rather than on the writing thread: these come from the transcription, which the
    // message thread owns, and a copy is cheap next to the decode that follows.
    auto* transcription_manager = mProcessor.getTranscriptionManager();

    request.source = mProcessor.getSourceAudioManager()->getSourceFile();
    request.notes = transcription_manager->getNoteEventVector();
    request.pedalEvents = transcription_manager->getPedalEvents();
    request.quantizeInfo = transcription_manager->getTimeQuantizeOptions().getTimeQuantizeInfo();
    request.exportBpm = mProcessor.getValueTree().getProperty(NnId::ExportTempoId, 120.0);
    request.pitchBendMode =
        static_cast<PitchBendModes>(mProcessor.getParameterValue(ParameterHelpers::PitchBendModeId));

    mSaveInFlight = true;
    mSaveButton->setEnabled(false);
    _setStatus("Saving " + request.stem, false);

    // Decoding a dropped file can take seconds, and doing that here would stop the window
    // painting and, in a plugin, the host's whole message loop with it.
    Component::SafePointer<SampleBar> safe_this(this);

    Thread::launch([request, safe_this]() {
        StringArray written;
        StringArray problems;

        runSave(request, written, problems);

        MessageManager::callAsync([safe_this, written, problems]() {
            if (auto* self = safe_this.getComponent())
                self->_finishSave(written, problems);
        });
    });
}

void SampleBar::_finishSave(const StringArray& written, const StringArray& problems)
{
    mSaveInFlight = false;
    mNextBaseNameStale = true;
    _updateEnablements();

    if (problems.isEmpty()) {
        _setStatus("Saved " + written.joinIntoString(" and "), false);
        return;
    }

    // Half a take is still worth keeping, so the half that landed is named alongside the
    // half that did not, and neither is quietly thrown away.
    const auto trouble = problems.joinIntoString(". ");

    if (written.isEmpty()) {
        _setStatus(trouble, true);
        return;
    }

    _setStatus("Saved " + written.joinIntoString(" and ") + ". " + trouble, true);
}

void SampleBar::_setStatus(const String& inText, bool inIsError)
{
    mShowingResult = true;
    // RECORD_RED is a graphic colour: 3.71:1 on a panel is fine for the record light but
    // under the 4.5:1 this label owes as text. DESTRUCTIVE is the same hue at 5.60:1.
    //
    // .hot rather than .base for the same reason. The accent is a graphic colour too, and
    // four of the eight the user can pick (magenta, rose, violet, orange) sit between 3.59
    // and 4.49 against a panel or a control. .hot is the same hue and clears 4.5:1 on every
    // accent, worst case 6.52. See docs/UI.md.
    mStatusLabel->setColour(Label::textColourId,
                            inIsError ? quarry::lnf::DESTRUCTIVE : okstudio::obsidian::accentOf(*this).hot);
    mStatusLabel->setText(inText, dontSendNotification);
}

void SampleBar::_updateEnablements()
{
    const bool has_take = mProcessor.getState() == PopulatedAudioAndMidiRegions;
    const bool any_format = mWavToggle->getToggleState() || mMidiToggle->getToggleState();

    // The timer runs through here four times a second, so without the in-flight test it would
    // put the button back under a write that has not finished.
    mSaveButton->setEnabled(has_take && any_format && !mSaveInFlight);

    const auto folder = _folder();
    // The button says where takes go, which is the fact you want on screen but a poor name to
    // hear read out. The title says what the control does; the tooltip has the path in full.
    //
    // The last two components rather than the whole path, because the whole path is the one
    // thing in this bar with no length at all: every other control is a word or a number this
    // file chose, and this one is whatever directory a person picked, which can be six
    // characters or two hundred. A button given a path too long for it wraps to two lines
    // inside thirty pixels rather than eliding, so a layout that only works while the path
    // stays short is not a layout. "Music\Quarry Samples" is what a person recognises anyway;
    // the tooltip has the rest, and the button beside it goes there.
    mFolderButton->setButtonText(_folderLabel(folder));
    mFolderButton->setTitle("Change folder");
    mFolderButton->setTooltip(folder.getFullPathName());

    if (mShowingResult)
        return;

    mStatusLabel->setColour(Label::textColourId, TEXT_DIM);

    if (!has_take) {
        mStatusLabel->setText("Record or drop something to save.", dontSendNotification);
    } else if (!any_format) {
        mStatusLabel->setText("Pick a format.", dontSendNotification);
    } else {
        if (mNextBaseNameStale || mNextBaseNameFolder != folder.getFullPathName())
            _refreshNextBaseName(folder);

        mStatusLabel->setText("Next: " + mNextBaseName, dontSendNotification);
    }
}

void SampleBar::timerCallback()
{
    const auto state = mProcessor.getState();

    // A new take supersedes whatever the last save said.
    if (state != mPreviousState) {
        mPreviousState = state;
        mShowingResult = false;
        mNextBaseNameStale = true;
    }

    _updateEnablements();
}

void SampleBar::paint(Graphics& g)
{
    okstudio::obsidian::raisedFill(g, getLocalBounds().toFloat().reduced(0.5f), 5.0f, PANEL_TOP, PANEL_BOT);

    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("SAVE TO", 14, 0, 70, getHeight(), Justification::centredLeft);
}

void SampleBar::resized()
{
    namespace L = SampleBarLayout;

    auto area = getLocalBounds().reduced(L::MARGIN_X, L::MARGIN_Y);
    area.removeFromLeft(L::SAVE_TO_LABEL); // the SAVE TO label painted above

    mSaveButton->setBounds(area.removeFromRight(L::SAVE_BUTTON));
    area.removeFromRight(L::SAVE_GAP);
    mMidiToggle->setBounds(area.removeFromRight(L::MIDI_TOGGLE));
    mWavToggle->setBounds(area.removeFromRight(L::WAV_TOGGLE));
    area.removeFromRight(L::PITCH_BEND_GAP);

    mPitchBend->setBounds(area.removeFromRight(L::PITCH_BEND).withSizeKeepingCentre(L::PITCH_BEND, 20));
    area.removeFromRight(L::PITCH_BEND_TRAIL);

    // Both of these stretch, and on this bar they are the only two that do, so the split between
    // them is stated in SampleBarLayout and checked in Tests/sample_bar_test.h rather than being
    // whatever halving the leftovers happens to produce. The status gets its floor first: the
    // folder's full path is on its tooltip, and the status sentence is nowhere but here.
    mFolderButton->setBounds(area.removeFromLeft(L::folderWidth(getWidth())));
    area.removeFromLeft(L::OPEN_GAP);
    mOpenFolderButton->setBounds(area.removeFromLeft(L::OPEN_BUTTON));
    area.removeFromLeft(L::ACTIVITY_GAP);
    mActivityToggleButton->setBounds(area.removeFromLeft(L::ACTIVITY_BUTTON));
    area.removeFromLeft(L::MIDDLE_GAP);
    mStatusLabel->setBounds(area);
}
