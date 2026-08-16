//
// The Sample page: pick an application, record it, keep it.
//

#ifndef SamplePageView_h
#define SamplePageView_h

#include <JuceHeader.h>

#include "PluginProcessor.h"
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

    std::unique_ptr<FileChooser> mFileChooser;

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
