//
// Keeping a take: a folder you pick once, two format toggles, and one button.
//

#include "SampleBar.h"

#include <okstudio/Obsidian.h>

namespace
{
constexpr int kMaxTakeNumber = 9999;
} // namespace

SampleBar::SampleBar(QuarryAudioProcessor& inProcessor)
    : mProcessor(inProcessor)
{
    mFolderButton = std::make_unique<TextButton>("FolderButton");
    mFolderButton->setTooltip("Choose where saved takes go.");
    mFolderButton->onClick = [this]() { _chooseFolder(); };
    addAndMakeVisible(*mFolderButton);

    mWavToggle = std::make_unique<ToggleButton>("Wav");
    mWavToggle->setToggleState(mProcessor.getValueTree().getProperty(NnId::SampleWriteWavId, true),
                               dontSendNotification);
    mWavToggle->setTooltip("Write the recorded audio.");
    mWavToggle->onClick = [this]() {
        mProcessor.getValueTree().setProperty(NnId::SampleWriteWavId, mWavToggle->getToggleState(), nullptr);
        _updateEnablements();
    };
    addAndMakeVisible(*mWavToggle);

    mMidiToggle = std::make_unique<ToggleButton>("Midi");
    mMidiToggle->setToggleState(mProcessor.getValueTree().getProperty(NnId::SampleWriteMidiId, true),
                                dontSendNotification);
    mMidiToggle->setTooltip("Write the transcription.");
    mMidiToggle->onClick = [this]() {
        mProcessor.getValueTree().setProperty(NnId::SampleWriteMidiId, mMidiToggle->getToggleState(), nullptr);
        _updateEnablements();
    };
    addAndMakeVisible(*mMidiToggle);

    mSaveButton = std::make_unique<TextButton>("Save");
    mSaveButton->setTooltip("Write this take to the folder on the left.");
    mSaveButton->onClick = [this]() { _save(); };
    addAndMakeVisible(*mSaveButton);

    mStatusLabel = std::make_unique<Label>("SampleStatus");
    mStatusLabel->setJustificationType(Justification::centredRight);
    mStatusLabel->setColour(Label::textColourId, TEXT_FAINT);
    mStatusLabel->setInterceptsMouseClicks(false, false);
    addAndMakeVisible(*mStatusLabel);

    _updateEnablements();
    startTimerHz(4);
}

SampleBar::~SampleBar()
{
    stopTimer();
}

File SampleBar::_folder() const
{
    const String stored = mProcessor.getValueTree().getProperty(NnId::SampleFolderId, String());

    if (stored.isNotEmpty())
        return File(stored);

    return File::getSpecialLocation(File::userMusicDirectory).getChildFile("Quarry Samples");
}

String SampleBar::_nextBaseName() const
{
    // A dropped file keeps its own name, so a saved take is recognisable next to
    // the thing it came from. A recording gets the next free take number.
    const auto dropped = mProcessor.getSourceAudioManager()->getDroppedFilename();

    if (dropped.isNotEmpty())
        return File::createLegalFileName(dropped);

    const auto folder = _folder();

    for (int i = 1; i <= kMaxTakeNumber; i++) {
        const auto candidate = "quarry-take-" + String(i).paddedLeft('0', 3);

        if (!folder.getChildFile(candidate + ".wav").existsAsFile()
            && !folder.getChildFile(candidate + ".mid").existsAsFile()) {
            return candidate;
        }
    }

    return "quarry-take";
}

void SampleBar::_chooseFolder()
{
    mFileChooser = std::make_unique<FileChooser>("Where should saved takes go?", _folder());

    const auto flags = FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories;

    mFileChooser->launchAsync(flags, [this](const FileChooser& inChooser) {
        const auto chosen = inChooser.getResult();

        if (chosen == File())
            return;

        mProcessor.getValueTree().setProperty(NnId::SampleFolderId, chosen.getFullPathName(), nullptr);
        mShowingResult = false;
        _updateEnablements();
    });
}

void SampleBar::_save()
{
    const auto folder = _folder();

    if (!folder.isDirectory()) {
        const auto created = folder.createDirectory();

        if (created.failed()) {
            _setStatus("Could not create " + folder.getFullPathName(), true);
            return;
        }
    }

    const auto base = _nextBaseName();
    const bool want_wav = mWavToggle->getToggleState();
    const bool want_midi = mMidiToggle->getToggleState();

    StringArray written;

    if (want_wav) {
        const auto source = mProcessor.getSourceAudioManager()->getSourceFile();

        if (!source.existsAsFile()) {
            _setStatus("The recorded audio is no longer on disk.", true);
            return;
        }

        auto destination = folder.getChildFile(base + ".wav");
        destination = destination.getNonexistentSibling();

        if (!source.copyFileTo(destination)) {
            _setStatus("Could not write " + destination.getFileName(), true);
            return;
        }

        written.add(destination.getFileName());
    }

    if (want_midi) {
        auto destination = folder.getChildFile(base + ".mid");
        destination = destination.getNonexistentSibling();

        const double export_bpm = mProcessor.getValueTree().getProperty(NnId::ExportTempoId, 120.0);

        const bool ok = mMidiFileWriter.writeMidiFile(
            mProcessor.getTranscriptionManager()->getNoteEventVector(),
            destination,
            mProcessor.getTranscriptionManager()->getTimeQuantizeOptions().getTimeQuantizeInfo(),
            export_bpm,
            static_cast<PitchBendModes>(mProcessor.getParameterValue(ParameterHelpers::PitchBendModeId)));

        if (!ok) {
            _setStatus("Could not write " + destination.getFileName(), true);
            return;
        }

        written.add(destination.getFileName());
    }

    _setStatus("Saved " + written.joinIntoString(" and "), false);
}

void SampleBar::_setStatus(const String& inText, bool inIsError)
{
    mShowingResult = true;
    mStatusLabel->setColour(Label::textColourId, inIsError ? RECORD_RED : okstudio::obsidian::accentOf(*this).base);
    mStatusLabel->setText(inText, dontSendNotification);
}

void SampleBar::_updateEnablements()
{
    const bool has_take = mProcessor.getState() == PopulatedAudioAndMidiRegions;
    const bool any_format = mWavToggle->getToggleState() || mMidiToggle->getToggleState();

    mSaveButton->setEnabled(has_take && any_format);

    const auto folder = _folder();
    mFolderButton->setButtonText(folder.getFullPathName());
    mFolderButton->setTooltip(folder.getFullPathName());

    if (mShowingResult)
        return;

    mStatusLabel->setColour(Label::textColourId, TEXT_FAINT);

    if (!has_take)
        mStatusLabel->setText("Record or drop something to save.", dontSendNotification);
    else if (!any_format)
        mStatusLabel->setText("Pick a format.", dontSendNotification);
    else
        mStatusLabel->setText("Next: " + _nextBaseName(), dontSendNotification);
}

void SampleBar::timerCallback()
{
    const auto state = mProcessor.getState();

    // A new take supersedes whatever the last save said.
    if (state != mPreviousState) {
        mPreviousState = state;
        mShowingResult = false;
    }

    _updateEnablements();
}

void SampleBar::paint(Graphics& g)
{
    okstudio::obsidian::raisedFill(g, getLocalBounds().toFloat().reduced(0.5f), 5.0f, PANEL_TOP, PANEL_BOT);

    g.setColour(TEXT_FAINT);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText("SAVE TO", 14, 0, 70, getHeight(), Justification::centredLeft);
}

void SampleBar::resized()
{
    auto area = getLocalBounds().reduced(10, 8);
    area.removeFromLeft(74); // the SAVE TO label painted above

    mSaveButton->setBounds(area.removeFromRight(78));
    area.removeFromRight(10);
    mMidiToggle->setBounds(area.removeFromRight(66));
    mWavToggle->setBounds(area.removeFromRight(64));
    area.removeFromRight(10);

    // The folder needs the room; the status takes what is left of the middle.
    mFolderButton->setBounds(area.removeFromLeft(jmin(300, area.getWidth() / 2)));
    area.removeFromLeft(12);
    mStatusLabel->setBounds(area);
}
