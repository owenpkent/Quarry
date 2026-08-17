//
// The Sample page: pick an application, record it, keep it.
//

#ifndef SamplePageView_h
#define SamplePageView_h

#include <JuceHeader.h>

#include "PluginProcessor.h"
#include "Sampler/SampleLibrary.h"
#include "UIDefines.h"

#if JUCE_WINDOWS

#include <memory>
#include <vector>

namespace quarry::sampler
{
class SampleRecorder;
}

/**
 * Quarry's second page.
 *
 * The Transcribe page turns audio into MIDI. This one captures audio in the first place,
 * from a single application rather than from the whole machine, and keeps it with an honest
 * record of where it came from.
 *
 * Deliberately holds no Windows types. The recorder underneath reaches WASAPI, and
 * windows.h defines a Rectangle() that makes juce::Rectangle ambiguous in everything parsed
 * after it, so the recorder is held behind a pointer and its header is included only by
 * this view's .cpp. SourceRow exists for the same reason: it is what the kit's AudioSession
 * looks like once the COM has been left behind.
 */
class SamplePageView
    : public Component
    , public Timer
    , public ListBoxModel
{
public:
    explicit SamplePageView(QuarryAudioProcessor& inProcessor);

    ~SamplePageView() override;

    void resized() override;

    void paint(Graphics& g) override;

    /** The accent is per editor and user-selectable, so anything tinted with it is re-tinted
        here rather than set once at construction. */
    void lookAndFeelChanged() override;

    void timerCallback() override;

    //==========================================================================
    int getNumRows() override;

    void paintListBoxItem(int row, Graphics& g, int width, int height, bool isSelected) override;

    void listBoxItemClicked(int row, const MouseEvent& event) override;

    /** Called once a capture has been loaded into the transcriber, so the window can follow it
        there. This page knows nothing about where the other page lives; the view that owns
        both of them decides what following means. */
    std::function<void()> onTranscribeRequested;

    /** Called when the page starts or stops wanting a narrow window, so whoever owns the
        window can resize it. */
    std::function<void()> onPreferredWidthChanged;

    /** True when only the source rail is showing, so the window has no use for the rest of
        its width. */
    bool wantsNarrowWindow() const { return ! mShowCaptures; }

    /** How wide and how tall the page needs to be in that state. */
    int narrowContentWidth() const;
    int narrowContentHeight() const;

private:
    /** The library list is a second ListBox on the same page, and ListBoxModel cannot be
        implemented twice by one class. This is the second one. */
    struct LibraryModel : public ListBoxModel
    {
        explicit LibraryModel(SamplePageView& inOwner) : owner(inOwner) {}

        int getNumRows() override;
        void paintListBoxItem(int row, Graphics& g, int width, int height, bool isSelected) override;
        void listBoxItemClicked(int row, const MouseEvent& event) override;

        SamplePageView& owner;
    };

    friend struct LibraryModel;

    /** One thing you can record: an application, or one window of an application showing
        several. Two browser windows are two rows and one process. */
    struct SourceRow
    {
        uint32 processId = 0;
        String name;
        float volume = 1.0f;
        float peak = 0.0f;
        bool isPlaying = false;

        /** Where the row's meter actually sits, as opposed to `peak`, which is where it is
            heading. Carried across refreshes so the level glides between enumerations rather
            than stepping four times a second. */
        float shownPeak = 0.0f;

        /** Which window this row stands for, or 0 when the application showed none. */
        uint64 windowHandle = 0;
        String windowTitle;

        /** True when the application had more than one window, so the title is what tells
            this row apart from its siblings and the name no longer does. */
        bool oneOfSeveral = false;
    };

    void _refreshSources();

    void _toggleRecording();

    void _fixSelectedVolume();

    void _chooseFolder();

    /** The row currently selected, or nothing if the app it named has gone away. */
    const SourceRow* _selectedSource() const;

    void _updateEnablements();

    /** The block under the source list: what is about to be recorded, or the clock while it
        is being recorded. The rail used to hold six hundred pixels of nothing here. */
    void _paintSelection(Graphics& g);

    /** The column headings over the library, so its numbers say what they are. */
    void _paintLibraryColumns(Graphics& g);

    File _libraryRoot() const;

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<quarry::sampler::SampleRecorder> mRecorder;

    /** Rebuilds the dropdown from mSources. Only when the list changes shape: doing it every
        refresh would shut a menu under the pointer four times a second. */
    void _rebuildChooser();

    std::unique_ptr<ListBox> mSourceList;

    /** The same choice as the list, for the narrow window, where a list of one visible row
        and a great deal of nothing was most of what the window was. */
    std::unique_ptr<ComboBox> mSourceChooser;
    std::unique_ptr<TextButton> mRecordButton;
    std::unique_ptr<TextButton> mFixVolumeButton;
    std::unique_ptr<TextButton> mFolderButton;

    std::unique_ptr<LibraryModel> mLibraryModel;
    std::unique_ptr<ListBox> mLibraryList;
    std::unique_ptr<TextEditor> mSearchBox;
    std::unique_ptr<TextButton> mTranscribeButton;
    std::unique_ptr<TextButton> mRevealButton;
    std::unique_ptr<TextButton> mDeleteButton;

    std::unique_ptr<FileChooser> mFileChooser;

    void _rescanLibrary();

    /** Rebuilds what the captures panel is showing: the folders and takes inside the folder
        being browsed, or, when there is something in the search box, matches from everywhere. */
    void _rebuildBrowse();

    /** Descends into `folder`, or back up to its parent. Refuses anything outside the capture
        folder: what is out there is not this application's business. */
    void _enterFolder(const File& folder);

    /** Shows or hides the captures half of the page, and remembers which. */
    void _setCapturesVisible(bool shouldShow);

    /** 1 when there is a folder to go back up to, 0 otherwise. The row it accounts for is the
        ".." at the top of the list. */
    int _upRowCount() const;
    void _openSelectedInTranscribe();
    void _revealSelected();
    void _deleteSelected();
    const quarry::sampler::LibraryEntry* _selectedEntry() const;

    std::unique_ptr<TextButton> mCapturesButton;

    std::vector<quarry::sampler::LibraryEntry> mAllEntries;

    /** The takes on screen: those inside mBrowseFolder, or the search results. */
    std::vector<quarry::sampler::LibraryEntry> mShownEntries;
    int mSelectedEntryRow = -1;

    /** Which folder is being looked at, and the folders inside it that hold captures, with how
        many each holds. Derived from the scan rather than from another walk of the disk. */
    File mBrowseFolder;
    std::vector<File> mBrowseFolders;
    std::vector<int> mBrowseCounts;

    bool mShowCaptures = true;

    // The scan is disk work, so it happens off the message thread and the result is handed
    // back through this. A page that stalls on a cold cache would be a page nobody opens.
    std::unique_ptr<ThreadPool> mScanPool;
    std::atomic<bool> mScanRunning { false };

    std::vector<SourceRow> mSources;

    // A pid alone no longer names a row: one application can be several of them. The window
    // is what separates two rows that share a process.
    uint32 mSelectedPid = 0xfffffffeu; // nothingPid; zero is a real pid and cannot mean "none"
    uint64 mSelectedWindow = 0;

    // Laid out in resized() and drawn in paint(), so the two cannot drift apart.
    Rectangle<int> mHeaderBounds;
    Rectangle<int> mLibraryHeaderBounds;
    Rectangle<int> mLibraryColumnsBounds;
    Rectangle<int> mSelectionBounds;
    Rectangle<int> mStatusBounds;
    Rectangle<int> mMeterBounds;

    String mStatusText;

    /** What the meter is showing, per side. Not the raw peak: it decays towards it. */
    float mRecordLevel[2] { 0.0f, 0.0f };

    int mTicksSinceSources = 0;

    /** When the last tick ran, so the meter moves by elapsed time rather than by tick count.
        Zero until the first one. */
    double mLastTickMs = 0.0;

    // The tick the meter and the clock move on. Sixty times a second: the ballistics below
    // are smooth at any rate, but the screen can only show as many positions as it is given,
    // and at thirty the fall is a visible staircase.
    static constexpr int tickIntervalMs = 16;

    // How often the session list is re-enumerated, counted in ticks. Every refresh is a round
    // of COM, which is a good deal slower than a meter wants to be; the two used to share one
    // timer and the meter is what paid for it. Sixteen ticks lands on the 250 ms this was. If
    // the enumeration ever shows up in a profile, the fix is a cached enumerator in the kit.
    static constexpr int ticksPerSourceRefresh = 16;

    // Meter ballistics, as time constants rather than per-tick factors, so a late frame moves
    // the bar exactly as far as the two early frames it replaced. Rise almost at once, because
    // a meter that lags what it is metering is worse than none; fall away slowly, because a
    // transient that has vanished before the eye arrives was never shown at all.
    static constexpr float meterAttackSeconds = 0.010f;
    static constexpr float meterReleaseSeconds = 0.320f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplePageView)
};

#endif // JUCE_WINDOWS

#endif // SamplePageView_h
