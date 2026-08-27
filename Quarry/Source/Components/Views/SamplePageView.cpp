//
// The Sample page.
//

#include "SamplePageView.h"

#include "QuarryLookAndFeel.h"

#if JUCE_WINDOWS

#include "NnId.h"
#include "QuarryTooltips.h"
#include "SourceAudioManager.h"
#include <okstudio/Obsidian.h>

#include <algorithm>

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

/** The record button, which is the one thing this page is for and is sized to say so. */
constexpr int primaryButtonHeight = 46;

/** Wide enough for an application name and its meter, and no wider: everything past that
    belongs to the library, which is the half that actually grows. */
constexpr int railWidth = 340;
constexpr int columnGap = 14;
constexpr int headerHeight = 34;

/** The narrow window, which is the whole page when the captures are away: a dropdown, what is
    arriving, what is happening, and the one button. Sized to that and nothing more. */
constexpr int chooserHeight = 32;
constexpr int meterHeight = 14;
constexpr int capturesButtonHeight = 24;
constexpr int narrowWidth = 300;

/** The library's fixed columns. What is left over is what the capture was.

    Three, deliberately. The application was on every row and said the same thing on each,
    which is a column that costs a scan and returns nothing. Loudness and whether the source
    was guessed are both real, and both belong to using a sample rather than finding one; they
    stay in the sidecar. */
constexpr int whenWidth = 52;
constexpr int lengthWidth = 56;

/** The gutter an application's name sits in, to the left of its windows' titles.

    Wide enough for the names that actually turn up. It was 64, which truncated
    "wallpaper32" to "wallpaper..." and left a three-window group labelled with an
    ellipsis, which is the one thing the gutter exists to say. The titles pay for the
    extra, and they are already the wider column. */
constexpr int appGutterWidth = 78;

/** Where the group rule sits inside that gutter: far enough off the title to read as a
    margin rather than as an underline for the text beside it. */
constexpr int appGutterRuleInset = 6;

/** Two lines of it, because the one about an unsupported Windows is a sentence. */
constexpr int statusHeight = 34;

/** The synthetic first row: record the whole endpoint rather than one application. Not a
    real pid, and picked so it can never collide with one. */
constexpr uint32 everythingPid = 0xffffffffu;

/** Nothing picked yet.

    Zero cannot mean this. Zero is the System Idle Process, it is what the endpoint's own
    session reports, and it is what an uninitialised pid holds - so a page nobody had touched
    came up with the System row drawn as chosen and the record button lit. */
constexpr uint32 nothingPid = 0xfffffffeu;

/** The immediate child of `under` that `file` sits somewhere inside.

    An invalid File when `file` is directly in `under`, or not inside it at all. This is what
    turns a flat list of capture paths back into the one level of folders above them. */
File branchOf(const File& under, const File& file)
{
    if (! file.isAChildOf(under))
        return {};

    auto candidate = file.getParentDirectory();

    if (candidate == under)
        return {};

    while (candidate.getParentDirectory() != under)
    {
        const auto up = candidate.getParentDirectory();

        if (up == candidate) // the filesystem root, which `under` was not
            return {};

        candidate = up;
    }

    return candidate;
}

/** The application's name as it is read, rather than as the process table spells it. The
    extension is not something anyone picking a source is choosing between. */
String appLabel(const String& processName)
{
    return processName.endsWithIgnoreCase(".exe") ? processName.dropLastCharacters(4)
                                                  : processName;
}

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

/** The record meter: one lane per side.

    Two lanes rather than one, because a single bar cannot tell a centred signal from one
    that has quietly lost a channel, and losing a channel is exactly the failure worth seeing
    before a take rather than after it. */
void drawStereoMeter(Graphics& g, juce::Rectangle<int> bounds, const float* levels, Colour fill)
{
    const auto lane = (bounds.getHeight() - 2) / 2;

    if (lane < 1)
        return;

    for (int side = 0; side < 2; ++side)
        drawMeter(g, bounds.withHeight(lane).withY(bounds.getY() + side * (lane + 2)),
                  levels[side], fill);
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

    mSourceFilter = std::make_unique<TextEditor>("Filter sources");
    mSourceFilter->setTextToShowWhenEmpty("filter windows", TEXT_DIM);
    mSourceFilter->setColour(TextEditor::backgroundColourId, PANEL_BOT);
    mSourceFilter->setColour(TextEditor::textColourId, TEXT_MAIN);
    mSourceFilter->setColour(TextEditor::outlineColourId, Colours::transparentBlack);
    mSourceFilter->onTextChange = [this]() { _applySourceFilter(); };
    addAndMakeVisible(*mSourceFilter);

    // The same choice the list offers, for the window that has no room for a list. Added as a
    // child component rather than made visible: only one of the two is ever on screen.
    mSourceChooser = std::make_unique<ComboBox>("Source");
    mSourceChooser->setTextWhenNothingSelected("Pick what to record");
    mSourceChooser->setTextWhenNoChoicesAvailable("No windows open");
    mSourceChooser->onChange = [this]() {
        const auto id = mSourceChooser->getSelectedId();

        if (id == 1)
        {
            mSelectedPid = everythingPid;
            mSelectedWindow = 0;
        }
        else if (id >= 2 && (size_t) (id - 2) < mShownSources.size())
        {
            const auto& picked = _shownSource(id - 2);
            mSelectedPid = picked.processId;
            mSelectedWindow = picked.windowHandle;
        }

        _updateEnablements();
        repaint();
    };
    addChildComponent(*mSourceChooser);

    mRecordButton = std::make_unique<TextButton>("RECORD");
    quarry::lnf::setRole(*mRecordButton, quarry::lnf::Role::primary);
    mRecordButton->setTitle("Record");
    mRecordButton->onClick = [this]() { _toggleRecording(); };
    addAndMakeVisible(*mRecordButton);

    mFixVolumeButton = std::make_unique<TextButton>("SET TO 100%");
    mFixVolumeButton->setTitle("Set volume to 100%");
    mFixVolumeButton->setColour(TextButton::buttonColourId, CONTROL_BG);
    mFixVolumeButton->setColour(TextButton::textColourOffId, TEXT_MAIN);
    mFixVolumeButton->onClick = [this]() { _fixSelectedVolume(); };
    addAndMakeVisible(*mFixVolumeButton);

    // Text, not a filled button: where captures are kept is a setting, and it was sitting at
    // the same visual weight as the list it describes. The path is on the tooltip, which is
    // where a fact you need once belongs.
    mFolderButton = std::make_unique<TextButton>("change folder");
    quarry::lnf::setRole(*mFolderButton, quarry::lnf::Role::quiet);
    mFolderButton->setTitle("Change folder");
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
    mSearchBox->onTextChange = [this]() { _rebuildBrowse(); };
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

    // Quiet, like the folder control beside it: showing the captures at all is a preference,
    // not part of the job the page is doing.
    mCapturesButton = std::make_unique<TextButton>("hide captures");
    quarry::lnf::setRole(*mCapturesButton, quarry::lnf::Role::quiet);
    mCapturesButton->setTitle("Show or hide captures");
    mCapturesButton->onClick = [this]() { _setCapturesVisible(! mShowCaptures); };
    addAndMakeVisible(*mCapturesButton);

    mScanPool = std::make_unique<ThreadPool>(1);

    if (! WasapiProcessLoopback::isSupported())
        mStatusText = "This version of Windows cannot record a single application.";
    else
        mStatusText = "Pick an application, then record.";

    mBrowseFolder = _libraryRoot();

    _refreshSources();
    _rescanLibrary();

    // Last, because it lays the page out for whichever way it was left.
    _setCapturesVisible((bool) mProcessor.getValueTree()
                            .getProperty(NnId::CaptureBrowserVisibleId, true));

    _updateEnablements();

    startTimer(tickIntervalMs);
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

    // Side by side rather than stacked halves. The sources are however many applications
    // happen to be making a sound, which is a handful; the library only ever grows. Splitting
    // the height evenly gave the short list the same room as the long one and got worse with
    // every capture, so the sources take a fixed rail and the library takes the rest.
    // With the captures turned off this is the whole page: a dropdown instead of the list,
    // and nothing that is not the act of recording. The window is sized to exactly this.
    if (! mShowCaptures)
    {
        auto column = area.reduced(4, 0);

        mHeaderBounds = {};
        mSourceFilter->setBounds({});
        mLibraryHeaderBounds = {};
        mLibraryColumnsBounds = {};
        mSelectionBounds = {};

        mSourceChooser->setBounds(column.removeFromTop(chooserHeight));
        column.removeFromTop(12);

        mCapturesButton->setBounds(column.removeFromBottom(capturesButtonHeight)
                                       .removeFromRight(130));
        column.removeFromBottom(8);

        mRecordButton->setBounds(column.removeFromBottom(primaryButtonHeight));
        column.removeFromBottom(10);

        mStatusBounds = column.removeFromBottom(statusHeight);
        column.removeFromBottom(4);

        mMeterBounds = column.removeFromBottom(meterHeight);
        return;
    }

    auto rail = area.removeFromLeft(railWidth);
    area.removeFromLeft(columnGap);

    auto library = area;

    //==========================================================================
    // The rail: pick something making a sound, then record it.
    mHeaderBounds = rail.removeFromTop(headerHeight);
    rail.removeFromTop(4);

    // From the foot upwards, so the record button keeps one place on the page however many
    // applications are listed above it. It is the only button down here: the page has one
    // action, and anything else standing beside it was competing with it for nothing.
    mRecordButton->setBounds(rail.removeFromBottom(primaryButtonHeight));
    rail.removeFromBottom(14);

    mStatusBounds = rail.removeFromBottom(statusHeight);
    rail.removeFromBottom(4);

    // The record meter sits directly above the status line, so what is arriving and what is
    // being said about it are read in one glance. Tall enough for two lanes and the gap that
    // keeps them apart.
    mMeterBounds = rail.removeFromBottom(14).reduced(6, 0);
    rail.removeFromBottom(12);

    // Only ever on screen when the selected application is playing below full volume, which
    // is the only time it means anything. Laid out either way so its arrival never moves the
    // name above it.
    mFixVolumeButton->setBounds(rail.removeFromBottom(buttonHeight));
    rail.removeFromBottom(10);

    // What is about to be recorded, named in full. The row above elides it, and on two
    // windows of one application the elided tail is the part that says which.
    mSelectionBounds = rail.removeFromBottom(66);
    rail.removeFromBottom(12);

    // The list is everything open now, not the two things making a noise, so it needs the same
    // way in that the library has had all along.
    mSourceFilter->setBounds(rail.removeFromTop(26));
    rail.removeFromTop(6);

    // Everything left over, the way the library list takes everything left over. A list with
    // room to grow reads as a list; it was only a hole when nothing else was in the column.
    rail.removeFromBottom(rail.getHeight() % rowHeight);
    mSourceList->setBounds(rail);

    //==========================================================================
    // The library: everything captured so far.
    if (! mShowCaptures)
        return;

    mLibraryHeaderBounds = library.removeFromTop(headerHeight);

    mCapturesButton->setBounds(mLibraryHeaderBounds.removeFromRight(130));
    mLibraryHeaderBounds.removeFromRight(6);

    // Where the captures are kept is a setting, not content, so it stops looking like a
    // control of the same weight as the list. The path it changes is its tooltip.
    mFolderButton->setBounds(mLibraryHeaderBounds.removeFromRight(130));
    mLibraryHeaderBounds.removeFromRight(12);

    library.removeFromTop(4);

    auto libraryFooter = library.removeFromBottom(buttonHeight);
    library.removeFromBottom(8);

    mTranscribeButton->setBounds(libraryFooter.removeFromLeft(150));
    libraryFooter.removeFromLeft(10);
    mRevealButton->setBounds(libraryFooter.removeFromLeft(170));
    mDeleteButton->setBounds(libraryFooter.removeFromRight(120));

    mSearchBox->setBounds(library.removeFromTop(26));
    library.removeFromTop(8);

    mLibraryColumnsBounds = library.removeFromTop(18);
    library.removeFromTop(2);

    // A list box whose height is not a whole number of rows slices the last one through the
    // middle of its text, which reads as a rendering fault rather than as "there is more
    // below". Give back the remainder instead.
    library.removeFromBottom(library.getHeight() % rowHeight);
    mLibraryList->setBounds(library);
}

void SamplePageView::paint(Graphics& g)
{
    g.setColour(PANEL_BG);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT());

    if (mShowCaptures)
        g.drawText("WINDOWS", mHeaderBounds.withTrimmedLeft(6),
                   Justification::centredLeft, false);

    if (mShowCaptures)
    {
        // The count belongs to the library, so it is drawn over the library rather than over
        // the list on the other side of the page. The folder path used to be here too, and was
        // a setting sitting at the same weight as the content.
        auto libraryHeader = mLibraryHeaderBounds.withTrimmedLeft(10);

        const auto root = _libraryRoot();
        const auto scanning = mScanRunning.load();

        // Where you are, once you are anywhere but the top. A count of everything is the right
        // thing to say about the whole folder and the wrong thing to say about one day inside
        // it, which is what the browser is standing in.
        const auto where = scanning                  ? String("reading ...")
                         : mBrowseFolder != root     ? mBrowseFolder.getRelativePathFrom(root)
                                                     : String((int) mAllEntries.size()) + " captured";

        g.drawText("CAPTURES", libraryHeader.removeFromLeft(88), Justification::centredLeft, false);
        g.drawText(where, libraryHeader, Justification::centredLeft, true);
    }

    if (mShowCaptures)
    {
        _paintLibraryColumns(g);
        _paintSelection(g);
    }

    if (mRecorder != nullptr && mRecorder->isRecording())
        drawStereoMeter(g, mMeterBounds, mRecordLevel, okstudio::obsidian::accentOf(*this).base);

    g.setColour(TEXT_MAIN);
    g.setFont(UIDefines::LABEL_FONT());

    // Two lines: "this version of Windows cannot record a single application" is a sentence,
    // and neither the rail nor the narrow window is wide enough to say it on one. Inset both
    // sides, because with the captures away there is nothing to the right to stop it.
    g.drawFittedText(mStatusText, mStatusBounds.reduced(6, 0), Justification::bottomLeft, 2);
}

void SamplePageView::_paintLibraryColumns(Graphics& g)
{
    auto columns = mLibraryColumnsBounds.reduced(10, 0);

    if (columns.getWidth() < 200)
        return;

    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT().withHeight(11.0f));

    // Right to left, matching the row beneath, so a heading cannot drift off its column.
    g.drawText("LENGTH", columns.removeFromRight(lengthWidth), Justification::centredRight, false);
    columns.removeFromRight(10);

    g.drawText("WHEN", columns.removeFromLeft(whenWidth), Justification::centredLeft, false);
    columns.removeFromLeft(10);
    g.drawText("CAPTURE", columns, Justification::centredLeft, false);
}

void SamplePageView::_paintSelection(Graphics& g)
{
    auto area = mSelectionBounds.reduced(6, 0);

    if (area.getHeight() < 40)
        return;

    const auto recording = mRecorder != nullptr && mRecorder->isRecording();

    g.setColour(TEXT_DIM);
    g.setFont(UIDefines::LABEL_FONT().withHeight(11.0f));
    g.drawText(recording ? "RECORDING" : "SELECTED", area.removeFromTop(16),
               Justification::centredLeft, false);
    area.removeFromTop(8);

    if (recording)
    {
        // A clock rather than a sentence about seconds. The status line already says the
        // words; what a take needs at a glance is the number.
        const auto seconds = mRecorder->recordedSeconds();
        const auto minutes = (int) (seconds / 60.0);
        const auto clock = String(minutes).paddedLeft('0', 2) + ":"
                         + String(seconds - 60.0 * minutes, 1).paddedLeft('0', 4);

        g.setColour(TEXT_MAIN);
        g.setFont(Font(FontOptions(UIDefines::MONTSERRAT_SEMIBOLD())).withPointHeight(26.0f));
        g.drawText(clock, area.removeFromTop(38), Justification::centredLeft, false);
        return;
    }

    const auto everything = mSelectedPid == everythingPid;
    const auto* source = _selectedSource();

    if (! everything && source == nullptr)
    {
        g.setColour(TEXT_DIM);
        g.drawFittedText("Nothing picked yet.", area, Justification::topLeft, 1);
        return;
    }

    // The full window title, wrapped. The row above elides it, and with two windows of one
    // application the elided tail is exactly the part that says which of them this is.
    const auto title = everything ? String("Everything this computer plays")
                     : source->windowTitle.isNotEmpty() ? source->windowTitle
                                                        : appLabel(source->name);

    g.setColour(TEXT_MAIN);
    g.setFont(Font(FontOptions(UIDefines::MONTSERRAT_SEMIBOLD())).withPointHeight(12.0f));
    g.drawFittedText(title, area.removeFromTop(jmin(area.getHeight(), 54)), Justification::topLeft, 3);

    if (area.getHeight() < 20)
        return;

    area.removeFromTop(6);

    g.setFont(UIDefines::LABEL_FONT());
    g.setColour(TEXT_DIM);

    // Only when it adds something. On an application showing one window the title above is
    // already the process name, and saying it twice reads as a bug.
    const auto beneath = everything ? String("every application at once")
                       : appLabel(source->name) != title ? appLabel(source->name)
                                                         : String();

    if (beneath.isNotEmpty())
        g.drawText(beneath, area.removeFromTop(18), Justification::topLeft, true);

    if (! everything && source->hasAudioSession && source->volume < 0.999f && area.getHeight() >= 18)
    {
        g.setColour(Colours::orange);
        g.drawText("playing at " + String(roundToInt(source->volume * 100.0f)) + "%, which is baked in",
                   area.removeFromTop(18), Justification::topLeft, true);
    }

    // Arming something silent is now allowed, so it has to be said. Otherwise a take started
    // ahead of a video reads as a broken recorder for as long as the video takes to start.
    if (! everything && ! source->hasAudioSession && area.getHeight() >= 18)
    {
        g.setColour(TEXT_DIM);
        g.drawText("silent for now, records when it plays", area.removeFromTop(18),
                   Justification::topLeft, true);
    }
}

void SamplePageView::lookAndFeelChanged()
{
    // Nothing to do any more. The accent used to be pushed onto the record button here so
    // that changing it repainted the page; Role::primary now resolves both the fill and the
    // text through accentOf() at paint time, which re-reads it on every repaint anyway.
    //
    // The override stays because a future colour that is not role-derived would go here.
}

//==============================================================================
void SamplePageView::timerCallback()
{
    const auto recording = mRecorder != nullptr && mRecorder->isRecording();

    // Real elapsed time, not the interval the timer was asked for. A message thread that has
    // just been busy delivers a late tick, and a meter that moved a fixed step per tick would
    // stall and then lurch; this one covers the ground the missed frames would have.
    const auto now = Time::getMillisecondCounterHiRes();
    const auto elapsed = mLastTickMs > 0.0 ? jlimit(0.001, 0.25, (now - mLastTickMs) * 0.001)
                                           : tickIntervalMs * 0.001;
    mLastTickMs = now;

    // One pole towards whatever arrived, fast up and slow down. Snapping straight to the
    // peak and multiplying it down each tick is the same idea done coarsely: it steps
    // rather than slides, and it steps differently whenever the frame rate wobbles.
    const auto follow = [elapsed](float& shown, float target)
    {
        const auto tau = target > shown ? meterAttackSeconds : meterReleaseSeconds;
        shown += (target - shown) * (1.0f - std::exp(-(float) elapsed / tau));
    };

    if (recording)
    {
        const auto peaks = mRecorder->readPeaks();

        follow(mRecordLevel[0], peaks.left);
        follow(mRecordLevel[1], peaks.right);

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
    else
    {
        mRecordLevel[0] = 0.0f;
        mRecordLevel[1] = 0.0f;
    }

    // The row meters run on the same ballistics as the record meter. What is behind them only
    // arrives with each COM enumeration, four times a second, so without this they step; with
    // it they slide between the readings and read as levels rather than as samples of one.
    auto anySourceMoving = false;

    for (auto& row : mSources)
    {
        follow(row.shownPeak, row.peak);
        anySourceMoving |= row.shownPeak > 0.001f;
    }

    if (++mTicksSinceSources >= ticksPerSourceRefresh)
    {
        mTicksSinceSources = 0;
        _refreshSources();
        repaint();
        return;
    }

    // Between enumerations only the parts that moved are redrawn. Repainting the library
    // sixty times a second to slide a meter would be a core spent on pixels that did not
    // change; a silent machine repaints nothing at all.
    if (recording)
    {
        repaint(mMeterBounds);
        repaint(mSelectionBounds);
        repaint(mStatusBounds);
    }

    if (anySourceMoving)
        mSourceList->repaint();
}

void SamplePageView::_refreshSources()
{
    if (! WasapiProcessLoopback::isSupported())
        return;

    std::vector<SourceRow> found;

    // Pass one: what is holding an audio session. These are the only rows that can carry a
    // meter or a volume warning, so they are gathered first and the window pass defers to them.
    //
    // Nothing is filtered on level any more. It used to be, because the list was the only way
    // to reach a source and a hundred silent sessions would have buried the one playing; now
    // the list holds every window on the desktop anyway and the filter box is how you get
    // through it. What loudness still decides is the order.
    const auto self = (uint32) GetCurrentProcessId();

    for (const auto& session : WasapiProcessLoopback::sessions())
    {
        // Quarry holds a session of its own for the preview synth, and offering to record
        // ourselves would be a loop with a menu item.
        if (session.processId == self)
            continue;

        SourceRow row;
        row.processId       = session.processId;
        row.name            = session.processName;
        row.volume          = session.volume;
        row.peak            = session.peak;
        row.isPlaying       = session.isPlaying;
        row.hasAudioSession = true;

        // An application showing several windows gets a row for each of them. Which one made
        // the sound is not a question the pid can answer - a browser is one process behind all
        // of its windows - so the choice is offered here instead of guessed at later.
        //
        // This is on the refresh timer, so it is paid four times a second. An EnumWindows pass
        // is cheap; the process snapshot behind parentOf() is not, and is only reached for a
        // session whose own pid owns no window. The desktop pass below adds one EnumWindows and
        // one OpenProcess per distinct process. If any of it ever shows up in a profile, the
        // fix is one snapshot per refresh shared across every session rather than one per.
        const auto windows = quarry::sampler::windowsOfSource(session.processId);

        if (windows.empty())
        {
            found.push_back(row);
            continue;
        }

        for (const auto& window : windows)
        {
            auto perWindow = row;
            perWindow.windowHandle = window.handle;
            perWindow.windowTitle  = window.title;
            found.push_back(perWindow);
        }
    }

    // Pass two: everything else that is open. Process loopback does not need its target to be
    // audible - the stream is a clock and a silent process still delivers - so a paused tab is
    // a thing you can arm the recorder on and then go press play. That is the normal way round
    // and the old list could not express it at all.
    for (const auto& window : quarry::sampler::desktopWindows())
    {
        const auto covered = std::any_of(found.begin(), found.end(), [&](const SourceRow& row) {
            return row.windowHandle == window.handle;
        });

        if (covered)
            continue;

        SourceRow row;
        row.processId   = window.processId;
        row.name        = window.processName.isNotEmpty() ? window.processName
                                                          : String("pid ") + String((int) window.processId);
        row.windowHandle = window.handle;
        row.windowTitle  = window.title;

        found.push_back(std::move(row));
    }

    // Loud first, then anything else with a session, then the rest by name. Sorted by process
    // rather than row by row, so an application's windows stay together whatever their levels
    // are doing: the grouped painting below reads them as adjacent and so does the eye.
    struct Group
    {
        uint32 processId = 0;
        float peak = 0.0f;
        bool hasAudioSession = false;
        String name;
    };

    std::vector<Group> groups;

    for (const auto& row : found)
    {
        auto existing = std::find_if(groups.begin(), groups.end(),
                                     [&](const Group& g) { return g.processId == row.processId; });

        if (existing == groups.end())
        {
            groups.push_back({row.processId, row.peak, row.hasAudioSession, row.name.toLowerCase()});
            continue;
        }

        existing->peak = jmax(existing->peak, row.peak);
        existing->hasAudioSession |= row.hasAudioSession;
    }

    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        const auto audibleA = a.peak > 0.0001f;
        const auto audibleB = b.peak > 0.0001f;

        if (audibleA != audibleB)
            return audibleA;

        if (audibleA && a.peak != b.peak)
            return a.peak > b.peak;

        if (a.hasAudioSession != b.hasAudioSession)
            return a.hasAudioSession;

        return a.name < b.name;
    });

    std::vector<SourceRow> ordered;
    ordered.reserve(found.size());

    for (const auto& group : groups)
        for (const auto& row : found)
            if (row.processId == group.processId)
                ordered.push_back(row);

    // Which rows have to say a window title to be told apart, settled once the order is fixed.
    for (size_t i = 0; i < ordered.size(); ++i)
    {
        const auto siblings = std::count_if(ordered.begin(), ordered.end(), [&](const SourceRow& row) {
            return row.processId == ordered[i].processId;
        });

        ordered[i].oneOfSeveral = siblings > 1;
    }

    // Carry each row's meter position over from the last enumeration. Without this every
    // refresh would restart the level from zero and the bar would tick rather than move.
    for (auto& row : ordered)
        for (const auto& previous : mSources)
            if (previous.processId == row.processId && previous.windowHandle == row.windowHandle)
            {
                row.shownPeak = previous.shownPeak;
                break;
            }

    mSources = std::move(ordered);
    _applySourceFilter();
}

void SamplePageView::_applySourceFilter()
{
    const auto wanted = mSourceFilter != nullptr ? mSourceFilter->getText().trim() : String();

    std::vector<size_t> shown;
    shown.reserve(mSources.size());

    for (size_t i = 0; i < mSources.size(); ++i)
    {
        const auto& row = mSources[i];

        // The selection always survives the filter. Typing to find the next thing to record
        // should not silently drop what is already armed, or worse, what is being recorded.
        const auto isSelected = row.processId == mSelectedPid && row.windowHandle == mSelectedWindow;

        const auto matches = wanted.isEmpty() || isSelected
                          || row.name.containsIgnoreCase(wanted)
                          || row.windowTitle.containsIgnoreCase(wanted);

        if (matches)
            shown.push_back(i);
    }

    // Compared row for row, not by count. A window closing while another opens leaves the
    // length alone and every label wrong, which used to be a rare enough coincidence to
    // ignore and is not now that the list holds the whole desktop.
    //
    // Against identities rather than against mShownSources, which by now indexes a vector that
    // has already been replaced: an index survives the refresh, what it pointed at does not.
    std::vector<std::pair<uint32, uint64>> identities;
    identities.reserve(shown.size());

    for (const auto index : shown)
        identities.emplace_back(mSources[index].processId, mSources[index].windowHandle);

    const auto changedShape = identities != mShownIdentities;

    mShownSources = std::move(shown);
    mShownIdentities = std::move(identities);

    if (changedShape)
    {
        _rebuildChooser();
        mSourceList->updateContent();

        // The list is as tall as its contents, so a row arriving or leaving moves everything
        // under it. Without this the detail block below would keep the old list's height.
        resized();
    }
    else
    {
        mSourceList->repaint(); // Same rows, new meters: no need to rebuild the list.
    }

    _updateEnablements();
}

//==============================================================================
int SamplePageView::getNumRows()
{
    // One more than there are applications: row zero is "everything", which is always
    // offered because it is the fallback when nothing else here can work.
    return (int) mShownSources.size() + 1;
}

int SamplePageView::_sourceBandIndex(int inRow) const
{
    // Row zero is "everything", which belongs to no application and takes band zero. The
    // rest count a new band each time the process changes, walking from the top so the
    // phase follows the data and not wherever the viewport happens to start.
    if (inRow <= 0)
        return 0;

    auto band = 1;

    for (auto i = 1; i < inRow; ++i)
        if (_shownSource(i).processId != _shownSource(i - 1).processId)
            ++band;

    return band;
}

void SamplePageView::paintListBoxItem(int row, Graphics& g, int width, int height, bool)
{
    if (! isPositiveAndBelow(row, getNumRows()))
        return;

    const auto isEverything = row == 0;
    const auto chosen = isEverything
                          ? mSelectedPid == everythingPid
                          : _shownSource(row - 1).processId == mSelectedPid
                                && _shownSource(row - 1).windowHandle == mSelectedWindow;

    const auto fullRow = juce::Rectangle<int>(0, 0, width, height);

    // The band is one application, not one row. See listRowBackground for why.
    quarry::lnf::listRowBackground(g, fullRow, _sourceBandIndex(row), chosen,
                                   okstudio::obsidian::accentOf(*this).base);

    auto bounds = fullRow.reduced(2, 1);

    // The accent bar takes the left three pixels of a selected row, so the text starts
    // after it rather than under it. Unselected rows keep the same indent so nothing
    // shifts sideways as the selection moves.
    bounds.removeFromLeft(3);

    g.setColour(TEXT_MAIN);
    g.setFont(UIDefines::LABEL_FONT());

    if (isEverything)
    {
        // The caveat trails the choice it applies to. Right-aligned in its own column it sat
        // level with the header above and read as a label for the whole list.
        auto text = bounds.reduced(8, 0);
        const auto label = String("Everything this computer plays");

        const auto labelWidth = GlyphArrangement::getStringWidthInt(g.getCurrentFont(), label);
        g.drawText(label, text.removeFromLeft(labelWidth + 12), Justification::centredLeft, true);

        // "guessed", not a sentence about it: the rail is narrow, and this is the same word
        // the library puts on the captures that come out of this choice.
        g.setColour(TEXT_DIM);
        g.drawText("source guessed", text, Justification::centredLeft, true);
        return;
    }

    const auto& source = _shownSource(row - 1);

    auto text = bounds.reduced(8, 0);
    auto meter = text.removeFromRight(80).withSizeKeepingCentre(80, 6);
    text.removeFromRight(10);

    // The volume the app is set to is baked into anything captured from it, so it is said
    // here rather than discovered afterwards in a file that came back quiet.
    if (source.hasAudioSession && source.volume < 0.999f)
    {
        auto warning = text.removeFromRight(70);
        g.setColour(Colours::orange);
        g.drawText(String(roundToInt(source.volume * 100.0f)) + "%", warning,
                   Justification::centredRight, false);
        g.setColour(chosen ? TEXT_MAIN : TEXT_DIM);
    }

    const auto accent = chosen ? okstudio::obsidian::accentOf(*this).base : TEXT_DIM;

    // A window with no audio session has no level, and an empty meter track sitting there
    // would read as silence rather than as "nothing to meter". Most of the list is in that
    // state now, so the track is drawn only where there is something to put in it.
    const auto paintMeter = [&](juce::Rectangle<int> inBounds) {
        if (source.hasAudioSession)
            drawMeter(g, inBounds, source.shownPeak, accent);
    };

    // One window, so the application is the row. Its name is the thing being picked, so it
    // takes TEXT_MAIN like a title would, and there is no gutter to rule off.
    if (! source.oneOfSeveral)
    {
        g.setColour(TEXT_MAIN);
        g.drawText(appLabel(source.name), text, Justification::centredLeft, true);
        paintMeter(meter);
        return;
    }

    // Several rows, one application. The title is the only thing that says which is which, so
    // it gets the room and it gets TEXT_MAIN; the name is printed once, against the first of
    // its windows, and left off the rest. Repeated down every row it was a column of
    // identical cells, which costs a read and settles nothing.
    //
    // What was missing was anything holding the group together, which left every row after
    // the first as an orphan with an empty column where the name would be. The band behind
    // the whole application and the rule down its gutter are that, and the name column keeps
    // its width on every row so the titles still line up.
    const auto index = row - 1;
    const auto firstOfGroup = index == 0 || _shownSource(index - 1).processId != source.processId;

    auto label = text.removeFromLeft(appGutterWidth);

    if (firstOfGroup)
    {
        g.setColour(TEXT_DIM);
        g.drawText(appLabel(source.name), label, Justification::centredLeft, true);
    }

    quarry::lnf::listGroupRule(g, fullRow, text.getX() - appGutterRuleInset);

    g.setColour(TEXT_MAIN);
    g.drawText(source.windowTitle, text, Justification::centredLeft, true);

    paintMeter(meter);
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
        mSelectedWindow = 0;
        mStatusText = "Ready to record everything. The source will be a guess.";
    }
    else
    {
        const auto& picked = _shownSource(row - 1);

        mSelectedPid = picked.processId;
        mSelectedWindow = picked.windowHandle;

        // Say the window back, because on a row that is one of several it is the only part
        // that identifies what was picked.
        const auto what = picked.oneOfSeveral && picked.windowTitle.isNotEmpty() ? picked.windowTitle
                                                                                 : picked.name;

        mStatusText = picked.hasAudioSession
                        ? "Ready to record " + what
                        : "Ready to record " + what + ". Nothing playing yet.";
    }

    _updateEnablements();
    repaint();
}

//==============================================================================
const SamplePageView::SourceRow& SamplePageView::_shownSource(int inListRow) const
{
    jassert(isPositiveAndBelow(inListRow, (int) mShownSources.size()));
    return mSources[mShownSources[(size_t) inListRow]];
}

const SamplePageView::SourceRow* SamplePageView::_selectedSource() const
{
    for (const auto& source : mSources)
        if (source.processId == mSelectedPid && source.windowHandle == mSelectedWindow)
            return &source;

    // The window closed while it was the selection. The application is still making a sound,
    // so fall back to it rather than dropping the choice out from under the record button.
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

    // Only on screen when it has something to fix. A permanent button for a rare problem is
    // a control that is dead almost always, which teaches you to stop looking at that corner
    // of the page; and when it did appear it had to explain itself, because nothing about a
    // standing "SET TO 100%" says what it acts on or why you would want it.
    // Not in the narrow window, which has room for the act of recording and nothing else.
    const auto quiet = mShowCaptures && ! recording && source != nullptr && source->hasAudioSession
                    && source->volume < 0.999f;

    mFixVolumeButton->setVisible(quiet);
    mFixVolumeButton->setEnabled(quiet);

    if (quiet)
        mFixVolumeButton->setButtonText("TURN " + appLabel(source->name).toUpperCase() + " UP TO 100%");

    // Everything is always recordable: it is the path that does not need process loopback,
    // and so the one that still works on a Windows too old for the rest of this page.
    mRecordButton->setEnabled(recording
                              || everything
                              || (source != nullptr && WasapiProcessLoopback::isSupported()));

    mFolderButton->setEnabled(! recording);

    mFolderButton->setTooltip("Captures are kept in " + _libraryRoot().getFullPathName());

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

        mRecordLevel[0] = 0.0f;
        mRecordLevel[1] = 0.0f;

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
    // fetched fresh here from the one session that matches. A window that holds no session
    // matches nothing, so the process is asked directly instead.
    for (const auto& live : WasapiProcessLoopback::sessions())
        if (live.processId == source->processId)
            session.executablePath = live.executablePath;

    if (session.executablePath.isEmpty())
        session.executablePath = quarry::sampler::executablePathOf(source->processId);

    // The window that was picked, so the sidecar records the one chosen rather than whichever
    // of the application's windows a guess would have landed on.
    const auto started = mRecorder->start(session, _libraryRoot(), source->windowHandle);

    const auto what = source->oneOfSeveral && source->windowTitle.isNotEmpty() ? source->windowTitle
                                                                              : source->name;

    mStatusText = started.wasOk() ? "Recording " + what + " ..." : started.getErrorMessage();

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
    return owner._upRowCount() + (int) owner.mBrowseFolders.size() + (int) owner.mShownEntries.size();
}

void SamplePageView::LibraryModel::paintListBoxItem(int row, Graphics& g, int width, int height, bool)
{
    const auto ups = owner._upRowCount();
    const auto folders = (int) owner.mBrowseFolders.size();

    if (! isPositiveAndBelow(row, getNumRows()))
        return;

    // Only a take can be the selection; the way back up and the folders are somewhere to
    // go, so they stripe like everything else but never take the accent bar.
    const auto entryRowForSelection = row - ups - folders;
    const auto rowIsChosen = entryRowForSelection >= 0 && entryRowForSelection == owner.mSelectedEntryRow;

    const auto fullRow = juce::Rectangle<int>(0, 0, width, height);
    quarry::lnf::listRowBackground(g, fullRow, row, rowIsChosen,
                                   okstudio::obsidian::accentOf(owner).base);

    auto bounds = fullRow.reduced(2, 1);
    bounds.removeFromLeft(3);

    g.setFont(UIDefines::LABEL_FONT());

    // The way back up, and the folders inside this one, both read as somewhere to go. Only the
    // takes below them are something to play, so only they can be the selection.
    if (row < ups + folders)
    {
        auto text = bounds.reduced(8, 0);

        g.setColour(TEXT_DIM);

        if (row < ups)
        {
            g.drawText("..", text.removeFromLeft(whenWidth), Justification::centredLeft, false);
            text.removeFromLeft(10);
            g.drawText("back to " + owner.mBrowseFolder.getParentDirectory().getFileName(), text,
                       Justification::centredLeft, true);
            return;
        }

        const auto index = (size_t) (row - ups);
        const auto held = owner.mBrowseCounts[index];

        g.drawText(String(held) + (held == 1 ? " take" : " takes"),
                   text.removeFromRight(lengthWidth + 30), Justification::centredRight, false);
        text.removeFromRight(10);

        g.drawText(">", text.removeFromLeft(whenWidth), Justification::centredLeft, false);
        text.removeFromLeft(10);

        g.setColour(TEXT_MAIN);
        g.drawText(owner.mBrowseFolders[index].getFileName(), text, Justification::centredLeft, true);
        return;
    }

    const auto entryRow = row - ups - folders;
    const auto& entry = owner.mShownEntries[(size_t) entryRow];
    const auto chosen = entryRow == owner.mSelectedEntryRow;

    // The fill and the accent bar are already down: listRowBackground drew them at the top
    // of this function, where it also knows the stripe phase.

    auto text = bounds.reduced(8, 0);

    // Right to left, so the numbers line up down the list however long the names are.
    g.setColour(TEXT_DIM);
    g.drawText(String(entry.durationSec, 1) + " s", text.removeFromRight(lengthWidth),
               Justification::centredRight, false);
    text.removeFromRight(10);

    // The time it was taken, rather than the digits the file name starts with. Same fact,
    // read without decoding it.
    g.drawText(entry.capturedAt.toString(false, true, false, true), text.removeFromLeft(whenWidth),
               Justification::centredLeft, false);
    text.removeFromLeft(10);

    // What the window said, which is what the capture is of. The file name is a slug built
    // from it and reads like one; it stays the name on disk and in search, not on the row.
    const auto name = entry.windowTitle.isNotEmpty() ? entry.windowTitle : entry.displayName();

    g.setColour(chosen ? TEXT_MAIN : TEXT_DIM);
    g.drawText(name, text, Justification::centredLeft, true);
}

void SamplePageView::LibraryModel::listBoxItemClicked(int row, const MouseEvent&)
{
    const auto ups = owner._upRowCount();
    const auto folders = (int) owner.mBrowseFolders.size();

    if (! isPositiveAndBelow(row, getNumRows()))
        return;

    if (row < ups)
    {
        owner._enterFolder(owner.mBrowseFolder.getParentDirectory());
        return;
    }

    if (row < ups + folders)
    {
        owner._enterFolder(owner.mBrowseFolders[(size_t) (row - ups)]);
        return;
    }

    const auto entryRow = row - ups - folders;
    owner.mSelectedEntryRow = entryRow;

    const auto& entry = owner.mShownEntries[(size_t) entryRow];
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
            safe->_rebuildBrowse();
        });
    });
}

int SamplePageView::narrowContentWidth() const
{
    return narrowWidth;
}

int SamplePageView::narrowContentHeight() const
{
    // Everything the narrow page lays out, plus the page's own inset. Spelled out rather than
    // measured so the window is sized before the layout runs rather than after it.
    return 4 + chooserHeight + 12 + meterHeight + 4 + statusHeight + 10 + primaryButtonHeight
         + 8 + capturesButtonHeight;
}

void SamplePageView::_rebuildChooser()
{
    if (mSourceChooser == nullptr)
        return;

    mSourceChooser->clear(dontSendNotification);
    mSourceChooser->addItem("Everything this computer plays", 1);

    for (size_t i = 0; i < mShownSources.size(); ++i)
    {
        const auto& source = mSources[mShownSources[i]];

        // The window title is what tells two of one application apart, so it is what the item
        // leads with; the name follows it for the ones showing a single window.
        auto label = source.oneOfSeveral && source.windowTitle.isNotEmpty()
                       ? appLabel(source.name) + "  " + source.windowTitle
                       : appLabel(source.name);

        // addItem refuses an empty string, and a process can come back without a name.
        if (label.trim().isEmpty())
            label = "pid " + String((int) source.processId);

        mSourceChooser->addItem(label, (int) i + 2);
    }

    // Follows the selection rather than carrying its own: the list and the dropdown are two
    // ways of saying the same thing and must not be able to disagree.
    int wanted = 0;

    if (mSelectedPid == everythingPid)
        wanted = 1;
    else
        for (size_t i = 0; i < mShownSources.size(); ++i)
            if (_shownSource((int) i).processId == mSelectedPid
                && _shownSource((int) i).windowHandle == mSelectedWindow)
                wanted = (int) i + 2;

    mSourceChooser->setSelectedId(wanted, dontSendNotification);
}

void SamplePageView::_setCapturesVisible(bool shouldShow)
{
    const auto was = mShowCaptures;

    mShowCaptures = shouldShow;
    mProcessor.getValueTree().setProperty(NnId::CaptureBrowserVisibleId, shouldShow, nullptr);

    mCapturesButton->setButtonText(shouldShow ? "hide captures" : "captures");

    for (auto* part : { static_cast<Component*>(mLibraryList.get()),
                        static_cast<Component*>(mSearchBox.get()),
                        static_cast<Component*>(mTranscribeButton.get()),
                        static_cast<Component*>(mRevealButton.get()),
                        static_cast<Component*>(mDeleteButton.get()),
                        static_cast<Component*>(mFolderButton.get()),
                        static_cast<Component*>(mSourceList.get()),
                        static_cast<Component*>(mSourceFilter.get()) })
        if (part != nullptr)
            part->setVisible(shouldShow);

    // The dropdown stands in for the list, so exactly one of them is up at a time.

    if (mSourceChooser != nullptr)
    {
        mSourceChooser->setVisible(! shouldShow);
        _rebuildChooser();
    }

    // Before laying out, because the window is about to change width and resized() should run
    // against the size the page is going to have rather than the one it is leaving.
    if (was != shouldShow && onPreferredWidthChanged)
        onPreferredWidthChanged();

    resized();
    repaint();
}

int SamplePageView::_upRowCount() const
{
    if (mSearchBox != nullptr && mSearchBox->getText().trim().isNotEmpty())
        return 0;

    return mBrowseFolder != _libraryRoot() && mBrowseFolder != File() ? 1 : 0;
}

void SamplePageView::_enterFolder(const File& folder)
{
    const auto root = _libraryRoot();

    // Never above the capture folder. A browser that can wander out into the rest of the disk
    // is a different and much larger promise than the one this makes.
    if (folder != root && ! folder.isAChildOf(root))
        return;

    mBrowseFolder = folder;
    _rebuildBrowse();
}

void SamplePageView::_rebuildBrowse()
{
    const auto root = _libraryRoot();

    if (mBrowseFolder == File() || (mBrowseFolder != root && ! mBrowseFolder.isAChildOf(root)))
        mBrowseFolder = root;

    mBrowseFolders.clear();
    mBrowseCounts.clear();
    mShownEntries.clear();
    mSelectedEntryRow = -1;

    const auto query = mSearchBox != nullptr ? mSearchBox->getText().trim() : String();

    // A search looks everywhere. Not knowing which folder a thing is in is the whole reason
    // anyone types in that box, so a query flattens the tree rather than filtering one level
    // of it; the folders come back when the box is emptied.
    if (query.isNotEmpty())
    {
        mShownEntries = quarry::sampler::SampleLibrary::filter(mAllEntries, query);
    }
    else
    {
        for (const auto& entry : mAllEntries)
        {
            if (entry.audioFile.getParentDirectory() == mBrowseFolder)
            {
                mShownEntries.push_back(entry);
                continue;
            }

            // Folders come out of what the scan already found, not out of another walk of the
            // disk, so one appears exactly when there is a capture somewhere inside it and
            // never when it is empty.
            const auto branch = branchOf(mBrowseFolder, entry.audioFile);

            if (branch == File())
                continue;

            const auto at = std::find(mBrowseFolders.begin(), mBrowseFolders.end(), branch);

            if (at == mBrowseFolders.end())
            {
                mBrowseFolders.push_back(branch);
                mBrowseCounts.push_back(1);
            }
            else
            {
                ++mBrowseCounts[(size_t) std::distance(mBrowseFolders.begin(), at)];
            }
        }
    }

    if (mLibraryList != nullptr)
    {
        mLibraryList->updateContent();
        mLibraryList->repaint();
    }

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
    {
        // Left in place for the return trip, which lands back on this page with the status
        // line still saying what happened.
        mStatusText = "Loaded " + entry->displayName() + " into Transcribe.";
        repaint();

        // Last, because it takes this page off screen.
        if (onTranscribeRequested)
            onTranscribeRequested();

        return;
    }

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
