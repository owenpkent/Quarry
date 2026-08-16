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

    void timerCallback() override;

    //==========================================================================
    int getNumRows() override;

    void paintListBoxItem(int row, Graphics& g, int width, int height, bool isSelected) override;

    void listBoxItemClicked(int row, const MouseEvent& event) override;

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

    /** One application with an audio session, as far as this page is concerned. */
    struct SourceRow
    {
        uint32 processId = 0;
        String name;
        float volume = 1.0f;
        float peak = 0.0f;
        bool isPlaying = false;
    };

    void _refreshSources();

    void _toggleRecording();

    void _fixSelectedVolume();

    void _chooseFolder();

    /** The row currently selected, or nothing if the app it named has gone away. */
    const SourceRow* _selectedSource() const;

    void _updateEnablements();

    File _libraryRoot() const;

    QuarryAudioProcessor& mProcessor;

    std::unique_ptr<quarry::sampler::SampleRecorder> mRecorder;

    std::unique_ptr<ListBox> mSourceList;
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
    void _applyFilter();
    void _openSelectedInTranscribe();
    void _revealSelected();
    void _deleteSelected();
    const quarry::sampler::LibraryEntry* _selectedEntry() const;

    std::vector<quarry::sampler::LibraryEntry> mAllEntries;
    std::vector<quarry::sampler::LibraryEntry> mShownEntries;
    int mSelectedEntryRow = -1;

    // The scan is disk work, so it happens off the message thread and the result is handed
    // back through this. A page that stalls on a cold cache would be a page nobody opens.
    std::unique_ptr<ThreadPool> mScanPool;
    std::atomic<bool> mScanRunning { false };

    std::vector<SourceRow> mSources;
    uint32 mSelectedPid = 0;

    // Laid out in resized() and drawn in paint(), so the two cannot drift apart.
    Rectangle<int> mHeaderBounds;
    Rectangle<int> mStatusBounds;
    Rectangle<int> mMeterBounds;

    String mStatusText;
    float mRecordLevel = 0.0f;

    // How often the session list is re-enumerated. Every refresh is a round of COM, so this
    // is a good deal slower than a meter wants to be. If it ever shows up in a profile, the
    // fix is a cached enumerator in the kit rather than a faster timer here.
    static constexpr int refreshIntervalMs = 250;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplePageView)
};

#endif // JUCE_WINDOWS

#endif // SamplePageView_h
