//
// The Sample page.
//

#include "SamplePageView.h"

#if JUCE_WINDOWS

#include "NnId.h"
#include "QuarryTooltips.h"
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

    if (! WasapiProcessLoopback::isSupported())
        mStatusText = "This version of Windows cannot record a single application.";
    else
        mStatusText = "Pick an application, then record.";

    _refreshSources();
    _updateEnablements();

    startTimer(refreshIntervalMs);
}

SamplePageView::~SamplePageView()
{
    stopTimer();

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
    g.drawText("APPLICATIONS PLAYING AUDIO", mHeaderBounds.withTrimmedLeft(6),
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
                                  repaint();
                              });
}

#endif // JUCE_WINDOWS
