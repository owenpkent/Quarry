//
// The Sample page.
//

#include "SamplePageView.h"

#if JUCE_WINDOWS

#include "NnId.h"
#include "QuarryTooltips.h"
#include "SourceAudioManager.h"
#include <okstudio/Obsidian.h>

// Last, deliberately: these drag in the Windows audio headers. Everything below them pays
// for it, which is why this file spells out juce::Rectangle and reaches for
// Colours::transparentBlack: windows.h declares a Rectangle() that makes the unqualified
// name ambiguous, and wingdi.h defines TRANSPARENT as the integer 1, which turns
// UIDefines' colour of that name into a number halfway through an argument list.
#include "Sampler/SampleRecorder.h"
#include <okstudio/WasapiProcessLoopback.h>

using okstudio::capture::WasapiProcessLoopback;

namespace
{
constexpr int rowHeight = 34; // The line's minimum hit target. Nothing here is smaller.
constexpr int buttonHeight = 36;

/** The synthetic first row: record the whole endpoint rather than one application. Not a
    real pid, and picked so it can never collide with one. */
constexpr uint32 everythingPid = 0xffffffffu;

/** A meter that reads as a level rather than as a bar chart, matching the SOURCE strip. */
void drawMeter(Graphics& g, juce::Rectangle<int> bounds, float level, Colour fill)
{
    g.setColour(PANEL_BOT);
    g.fillRoundedRectangle(bounds.toFloat(), 2.0f);

    const auto filled = jlimit(0.0f, 1.0f, level);

    if (filled > 0.001f)
    {
        auto lit = bounds.toFloat().withWidth(bounds.toFloat().getWidth() * filled);
        g.setColour(fill);
        g.fillRoundedRectangle(lit, 2.0f);
    }
}
} // namespace

//==============================================================================
SamplePageView::SamplePageView(QuarryAudioProcessor& inProcessor)
    : mProcessor(inProcessor)
{
    mRecorder = std::make_unique<quarry::sampler::SampleRecorder>();

    mSourceList = std::make_unique<ListBox>("Sources", this);
    mSourceList->setRowHeight(rowHeight);
    mSourceList->setColour(ListBox::backgroundColourId, PANEL_BOT);
    mSourceList->setColour(ListBox::outlineColourId, Colours::transparentBlack);
    addAndMakeVisible(*mSourceList);

    mRecordButton = std::make_unique<TextButton>("RECORD");
    mRecordButton->setColour(TextButton::buttonColourId, CONTROL_BG);
    mRecordButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mRecordButton->onClick = [this]() { _toggleRecording(); };
    addAndMakeVisible(*mRecordButton);

    mFixVolumeButton = std::make_unique<TextButton>("SET TO 100%");
    mFixVolumeButton->setColour(TextButton::buttonColourId, CONTROL_BG);
    mFixVolumeButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mFixVolumeButton->onClick = [this]() { _fixSelectedVolume(); };
    addAndMakeVisible(*mFixVolumeButton);

    mFolderButton = std::make_unique<TextButton>("FOLDER");
    mFolderButton->setColour(TextButton::buttonColourId, CONTROL_BG);
    mFolderButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mFolderButton->onClick = [this]() { _chooseFolder(); };
    addAndMakeVisible(*mFolderButton);

    //==========================================================================
    // The library: everything captured so far, read back off its own sidecars.
    mLibraryModel = std::make_unique<LibraryModel>(*this);

    mLibraryList = std::make_unique<ListBox>("Library", mLibraryModel.get());
    mLibraryList->setRowHeight(rowHeight);
    mLibraryList->setColour(ListBox::backgroundColourId, PANEL_BOT);
    mLibraryList->setColour(ListBox::outlineColourId, Colours::transparentBlack);
    addAndMakeVisible(*mLibraryList);

    mSearchBox = std::make_unique<TextEditor>("Search");
    mSearchBox->setTextToShowWhenEmpty("search captures", TEXT_DIM);
    mSearchBox->setColour(TextEditor::backgroundColourId, PANEL_BOT);
    mSearchBox->setColour(TextEditor::textColourId, TEXT_MAIN);
    mSearchBox->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    mSearchBox->onTextChange = [this]() { _applyFilter(); };
    addAndMakeVisible(*mSearchBox);

    const auto makeLibraryButton = [this](const String& inText, std::function<void()> inAction) {
        auto button = std::make_unique<TextButton>(inText);
        button->setColour(TextButton::buttonColourId, CONTROL_BG);
        button->setColour(TextButton::textColourOffId, TEXT_MAIN);
        button->onClick = std::move(inAction);
        addAndMakeVisible(*button);
        return button;
    };

    mTranscribeButton = makeLibraryButton("TRANSCRIBE", [this]() { _openSelectedInTranscribe(); });
    mRevealButton = makeLibraryButton("SHOW IN FOLDER", [this]() { _revealSelected(); });
    mDeleteButton = makeLibraryButton("DELETE", [this]() { _deleteSelected(); });

    mScanPool = std::make_unique<ThreadPool>(1);

    if (! WasapiProcessLoopback::isSupported())
        mStatusText = "This version of Windows cannot record a single application.";
    else
        mStatusText = "Pick an application, then record.";

    _refreshSources();
    _rescanLibrary();
    _updateEnablements();

    startTimer(refreshIntervalMs);
}

SamplePageView::~SamplePageView()
{
    stopTimer();

    // Before anything the scan job captures by reference goes away.
    if (mScanPool != nullptr)
        mScanPool->removeAllJobs(true, 4000);

    // Before the recorder is destroyed, so the capture thread is joined while the buffers it
    // writes into are still there to be written to.
    if (mRecorder != nullptr && mRecorder->isRecording())
        mRecorder->discard();
}

//==============================================================================
void SamplePageView::resized()
{
    auto area = getLocalBounds().reduced(2);

    mHeaderBounds = area.removeFromTop(26);
    area.removeFromTop(6);

    // The library takes the bottom half, laid out from the bottom up so the two halves meet
    // in the middle rather than the lower one being whatever is left over.
    auto library = area.removeFromBottom(area.getHeight() / 2);

    {
        auto libraryFooter = library.removeFromBottom(buttonHeight);
        library.removeFromBottom(6);

        mTranscribeButton->setBounds(libraryFooter.removeFromLeft(150));
        libraryFooter.removeFromLeft(10);
        mRevealButton->setBounds(libraryFooter.removeFromLeft(170));
        mDeleteButton->setBounds(libraryFooter.removeFromRight(120));

        mSearchBox->setBounds(library.removeFromTop(26));
        library.removeFromTop(6);
        mLibraryList->setBounds(library);
    }

    area.removeFromBottom(12);

    auto footer = area.removeFromBottom(buttonHeight + 12);
    footer.removeFromTop(12);

    mStatusBounds = area.removeFromBottom(24);
    area.removeFromBottom(4);

    // The record meter sits directly above the status line, so what is arriving and what is
    // being said about it are read in one glance.
    mMeterBounds = area.removeFromBottom(8);
    area.removeFromBottom(6);

    mSourceList->setBounds(area);

    mRecordButton->setBounds(footer.removeFromLeft(150));
    footer.removeFromLeft(10);
    mFixVolumeButton->setBounds(footer.removeFromLeft(140));
    mFolderButton->setBounds(footer.removeFromRight(220));
}

void SamplePageView::paint(Graphics& g)
{
    g.setColour(PANEL_BG);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());

    const auto scanning = mScanRunning.load();
    const auto captures = scanning ? String("reading ...")
                                   : String((int) mAllEntries.size()) + " captured";

    g.drawText("APPLICATIONS PLAYING AUDIO", mHeaderBounds.withTrimmedLeft(6),
               Justification::centredLeft, false);
    g.drawText(captures, mHeaderBounds.withTrimmedRight(6).withTrimmedLeft(300),
               Justification::centredLeft, false);

    const auto root = _libraryRoot();
    g.drawText(root.getFullPathName(), mHeaderBounds.withTrimmedRight(6),
               Justification::centredRight, true);

    if (mRecorder != nullptr && mRecorder->isRecording())
        drawMeter(g, mMeterBounds, mRecordLevel, okstudio::obsidian::accentOf(*this).base);

    g.setColour(TEXT_MAIN);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText(mStatusText, mStatusBounds.withTrimmedLeft(6), Justification::centredLeft, true);
}

//==============================================================================
void SamplePageView::timerCallback()
{
    if (mRecorder != nullptr && mRecorder->isRecording())
    {
        mRecordLevel = mRecorder->readPeak();

        const auto failure = mRecorder->streamFailure();

        if (failure.isNotEmpty())
        {
            // The source went away mid-take. Keep what was captured rather than throwing it
            // out: a sample that ends early is still a sample.
            _toggleRecording();
            mStatusText = failure;
            repaint();
            return;
        }

        mStatusText = "Recording " + String(mRecorder->recordedSeconds(), 1) + " s";
    }

    _refreshSources();
    repaint();
}

void SamplePageView::_refreshSources()
{
    if (! WasapiProcessLoopback::isSupported())
        return;

    std::vector<SourceRow> found;

    for (const auto& session : WasapiProcessLoopback::sessions())
    {
        // Everything holds a session; almost nothing is making a sound. Premiere, Resolve
        // and a wallpaper engine all sit at a peak of exactly zero waiting for the moment
        // they need it. Showing all of them would bury the one row that matters, so a row
        // has to be audible now, or be the one already chosen.
        const auto worthShowing = session.peak > 0.0001f || session.processId == mSelectedPid;

        if (! worthShowing)
            continue;

        SourceRow row;
        row.processId = session.processId;
        row.name      = session.processName;
        row.volume    = session.volume;
        row.peak      = session.peak;
        row.isPlaying = session.isPlaying;
        found.push_back(row);
    }

    const auto changedShape = found.size() != mSources.size();
    mSources = std::move(found);

    if (changedShape)
        mSourceList->updateContent();
    else
        mSourceList->repaint(); // Same rows, new meters: no need to rebuild the list.

    _updateEnablements();
}

//==============================================================================
int SamplePageView::getNumRows()
{
    // One more than there are applications: row zero is "everything", which is always
    // offered because it is the fallback when nothing else here can work.
    return (int) mSources.size() + 1;
}

void SamplePageView::paintListBoxItem(int row, Graphics& g, int width, int height, bool)
{
    if (! isPositiveAndBelow(row, getNumRows()))
        return;

    const auto isEverything = row == 0;
    const auto chosen = isEverything ? mSelectedPid == everythingPid
                                     : mSources[(size_t) row - 1].processId == mSelectedPid;

    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(2, 1);

    if (chosen)
    {
        g.setColour(CONTROL_BG);
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
    }

    g.setColour(chosen ? TEXT_MAIN : TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());

    if (isEverything)
    {
        auto text = bounds.reduced(8, 0);
        g.drawText("Everything this computer plays", text.removeFromLeft(text.getWidth() / 2),
                   Justification::centredLeft, true);

        g.setColour(TEXT_DIM);
        g.drawText("source can only be guessed at", text, Justification::centredRight, true);
        return;
    }

    const auto& source = mSources[(size_t) row - 1];

    auto text = bounds.reduced(8, 0);
    auto meter = text.removeFromRight(120).withSizeKeepingCentre(120, 6);
    text.removeFromRight(10);

    // The volume the app is set to is baked into anything captured from it, so it is said
    // here rather than discovered afterwards in a file that came back quiet.
    if (source.volume < 0.999f)
    {
        auto warning = text.removeFromRight(90);
        g.setColour(Colours::orange);
        g.drawText(String(roundToInt(source.volume * 100.0f)) + "% VOLUME", warning,
                   Justification::centredRight, false);
        g.setColour(chosen ? TEXT_MAIN : TEXT_DIM);
    }

    g.drawText(source.name, text, Justification::centredLeft, true);

    drawMeter(g, meter, source.peak, chosen ? okstudio::obsidian::accentOf(*this).base : TEXT_DIM);
}

void SamplePageView::listBoxItemClicked(int row, const MouseEvent&)
{
    if (! isPositiveAndBelow(row, getNumRows()))
        return;

    // Changing source mid-take would splice two applications into one file, so the choice is
    // locked while recording rather than silently ignored.
    if (mRecorder != nullptr && mRecorder->isRecording())
        return;

    if (row == 0)
    {
        mSelectedPid = everythingPid;
        mStatusText = "Ready to record everything. The source will be a guess.";
    }
    else
    {
        mSelectedPid = mSources[(size_t) row - 1].processId;
        mStatusText = "Ready to record " + mSources[(size_t) row - 1].name;
    }

    _updateEnablements();
    repaint();
}

//==============================================================================
const SamplePageView::SourceRow* SamplePageView::_selectedSource() const
{
    for (const auto& source : mSources)
        if (source.processId == mSelectedPid)
            return &source;

    return nullptr;
}

File SamplePageView::_libraryRoot() const
{
    const String stored = mProcessor.getValueTree().getProperty(NnId::CaptureFolderId, String());

    if (stored.isNotEmpty())
        return File(stored);

    return File::getSpecialLocation(File::userMusicDirectory).getChildFile("Quarry Captures");
}

void SamplePageView::_updateEnablements()
{
    const auto recording = mRecorder != nullptr && mRecorder->isRecording();
    const auto* source = _selectedSource();
    const auto everything = mSelectedPid == everythingPid;

    mRecordButton->setButtonText(recording ? "STOP" : "RECORD");

    // Everything is always recordable: it is the path that does not need process loopback,
    // and so the one that still works on a Windows too old for the rest of this page.
    mRecordButton->setEnabled(recording
                              || everything
                              || (source != nullptr && WasapiProcessLoopback::isSupported()));

    mFixVolumeButton->setEnabled(! recording && source != nullptr && source->volume < 0.999f);
    mFolderButton->setEnabled(! recording);

    const auto haveEntry = _selectedEntry() != nullptr;
    mTranscribeButton->setEnabled(haveEntry && ! recording);
    mRevealButton->setEnabled(haveEntry);
    mDeleteButton->setEnabled(haveEntry);
}

//==============================================================================
void SamplePageView::_toggleRecording()
{
    if (mRecorder == nullptr)
        return;

    if (mRecorder->isRecording())
    {
        const auto written = mRecorder->stop();

        mStatusText = written.ok ? written.message + "  ->  " + written.audioFile.getFileName()
                                 : written.message;

        mRecordLevel = 0.0f;

        // A take that just landed should be in the list without being asked for.
        if (written.ok)
            _rescanLibrary();

        _updateEnablements();
        repaint();
        return;
    }

    if (mSelectedPid == everythingPid)
    {
        const auto begun = mRecorder->startEndpoint(_libraryRoot());
        mStatusText = begun.wasOk() ? "Recording everything ..." : begun.getErrorMessage();

        _updateEnablements();
        repaint();
        return;
    }

    const auto* source = _selectedSource();

    if (source == nullptr)
    {
        mStatusText = "Pick an application first.";
        repaint();
        return;
    }

    okstudio::capture::AudioSession session;
    session.processId   = source->processId;
    session.processName = source->name;
    session.volume      = source->volume;
    session.peak        = source->peak;
    session.isPlaying   = source->isPlaying;

    // The executable path is not carried on the row, and the sidecar wants it, so it is
    // fetched fresh here from the one session that matches.
    for (const auto& live : WasapiProcessLoopback::sessions())
        if (live.processId == source->processId)
            session.executablePath = live.executablePath;

    const auto started = mRecorder->start(session, _libraryRoot());

    mStatusText = started.wasOk() ? "Recording " + source->name + " ..." : started.getErrorMessage();

    _updateEnablements();
    repaint();
}

void SamplePageView::_fixSelectedVolume()
{
    const auto* source = _selectedSource();

    if (source == nullptr)
        return;

    if (WasapiProcessLoopback::setSessionVolume(source->processId, 1.0f))
        mStatusText = "Set " + source->name + " back to full volume.";
    else
        mStatusText = "Could not change that application's volume.";

    _refreshSources();
    repaint();
}

//==============================================================================
// The library.

int SamplePageView::LibraryModel::getNumRows()
{
    return (int) owner.mShownEntries.size();
}

void SamplePageView::LibraryModel::paintListBoxItem(int row, Graphics& g, int width, int height, bool)
{
    if (! isPositiveAndBelow(row, (int) owner.mShownEntries.size()))
        return;

    const auto& entry = owner.mShownEntries[(size_t) row];
    const auto chosen = row == owner.mSelectedEntryRow;

    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(2, 1);

    if (chosen)
    {
        g.setColour(CONTROL_BG);
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
    }

    auto text = bounds.reduced(8, 0);

    // Right to left, so the numbers line up down the list however long the names are.
    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());
    g.drawText(String(entry.lufs, 1) + " LUFS", text.removeFromRight(90), Justification::centredRight, false);
    g.drawText(String(entry.durationSec, 1) + " s", text.removeFromRight(60), Justification::centredRight, false);

    if (! entry.isolatedToProcess)
        g.drawText("guessed", text.removeFromRight(70), Justification::centredRight, false);

    text.removeFromRight(10);

    g.setColour(chosen ? TEXT_MAIN : TEXT_DIM);
    g.drawText(entry.displayName(), text, Justification::centredLeft, true);
}

void SamplePageView::LibraryModel::listBoxItemClicked(int row, const MouseEvent&)
{
    if (! isPositiveAndBelow(row, (int) owner.mShownEntries.size()))
        return;

    owner.mSelectedEntryRow = row;

    const auto& entry = owner.mShownEntries[(size_t) row];
    owner.mStatusText = entry.windowTitle.isNotEmpty() ? entry.windowTitle : entry.displayName();

    owner._updateEnablements();
    owner.mLibraryList->repaint();
    owner.repaint();
}

const quarry::sampler::LibraryEntry* SamplePageView::_selectedEntry() const
{
    if (! isPositiveAndBelow(mSelectedEntryRow, (int) mShownEntries.size()))
        return nullptr;

    return &mShownEntries[(size_t) mSelectedEntryRow];
}

void SamplePageView::_rescanLibrary()
{
    if (mScanPool == nullptr || mScanRunning.exchange(true))
        return;

    const auto root = _libraryRoot();

    mScanPool->addJob([this, root]() {
        auto found = quarry::sampler::SampleLibrary::scan(root);

        // Back to the message thread to publish. The SafePointer is what makes closing the
        // window mid-scan safe rather than a race nobody would ever reproduce on purpose.
        Component::SafePointer<SamplePageView> safe(this);

        MessageManager::callAsync([safe, found = std::move(found)]() mutable {
            if (safe == nullptr)
                return;

            safe->mAllEntries = std::move(found);
            safe->mScanRunning.store(false);
            safe->_applyFilter();
        });
    });
}

void SamplePageView::_applyFilter()
{
    const auto query = mSearchBox != nullptr ? mSearchBox->getText() : String();

    mShownEntries = quarry::sampler::SampleLibrary::filter(mAllEntries, query);
    mSelectedEntryRow = -1;

    mLibraryList->updateContent();
    mLibraryList->repaint();

    _updateEnablements();
    repaint();
}

void SamplePageView::_openSelectedInTranscribe()
{
    const auto* entry = _selectedEntry();

    if (entry == nullptr)
        return;

    // The entire reason this page lives inside Quarry rather than beside it: a captured
    // sample is one click from the transcriber, and from the player that comes with it.
    if (auto* audio = mProcessor.getSourceAudioManager(); audio != nullptr && audio->onFileDrop(entry->audioFile))
        mStatusText = "Loaded " + entry->displayName() + " into Transcribe.";
    else
        mStatusText = "Could not load that sample into Transcribe.";

    repaint();
}

void SamplePageView::_revealSelected()
{
    if (const auto* entry = _selectedEntry())
        entry->audioFile.revealToUser();
}

void SamplePageView::_deleteSelected()
{
    const auto* entry = _selectedEntry();

    if (entry == nullptr)
        return;

    const auto name = entry->displayName();

    // To the recycle bin, not gone. A browser whose delete button is final is one you stop
    // trusting with anything you care about.
    mStatusText = quarry::sampler::SampleLibrary::remove(*entry)
                    ? "Moved " + name + " to the recycle bin."
                    : "Could not delete " + name + ".";

    _rescanLibrary();
    repaint();
}

void SamplePageView::_chooseFolder()
{
    mFileChooser = std::make_unique<FileChooser>("Where captured samples go", _libraryRoot());

    mFileChooser->launchAsync(FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                              [this](const FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();

                                  if (chosen == File())
                                      return;

                                  mProcessor.getValueTree().setProperty(NnId::CaptureFolderId,
                                                                        chosen.getFullPathName(),
                                                                        nullptr);
                                  _rescanLibrary();
                                  repaint();
                              });
}

#endif // JUCE_WINDOWS
