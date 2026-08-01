//
// Created by Damien Ronssin on 02.06.2024.
//

#ifndef TranscriptionManager_h
#define TranscriptionManager_h

#include <cstdint>

#include <JuceHeader.h>
#include "BasicPitch.h"
#include "NoteOptions.h"
#include "TimeQuantizeOptions.h"

class QuarryAudioProcessor;
class QuarryMainView;
class QuarryEditor;

class TranscriptionManager
    : public Timer
    , public AudioProcessorValueTreeState::Listener
{
public:
    explicit TranscriptionManager(QuarryAudioProcessor* inProcessor);

    void timerCallback() override;

    void prepareToPlay(double inSampleRate);

    void processBlock(int inNumSamples);

    void setLaunchNewTranscription();

    void launchTranscribeJob();

    void parameterChanged(const juce::String& parameterID, float newValue) override;

    bool isJobRunningOrQueued() const;

    const std::vector<Notes::Event>& getNoteEventVector() const;

    /**
     * The model output, before scale and time quantization have had a say. Anything that reports on
     * what was played rather than on what is being sent out wants this one.
     */
    const std::vector<Notes::Event>& getRawNoteEventVector() const;

    /**
     * Bumped every time the raw note events are rebuilt, so a reader can spot a change that leaves
     * the note count the same. Post-processing does not bump it: it does not touch these notes.
     */
    std::uint32_t getRawNoteEventsRevision() const;

    TimeQuantizeOptions& getTimeQuantizeOptions();

    void clear();

private:
    void _runModel();

    void _updateTranscription();

    void _updatePostProcessing();

    void _repaintPianoRoll();

    QuarryAudioProcessor* mProcessor;

    BasicPitch mBasicPitch;
    NoteOptions mNoteOptions;
    TimeQuantizeOptions mTimeQuantizeOptions;

    std::vector<Notes::Event> mPostProcessedNotes;

    std::atomic<std::uint32_t> mRawNoteEventsRevision {0};

    std::atomic<bool> mShouldRunNewTranscription = false;
    std::atomic<bool> mShouldUpdateTranscription = false;
    std::atomic<bool> mShouldUpdatePostProcessing = false;
    std::atomic<bool> mShouldRepaintPianoRoll = false;

    ThreadPool mThreadPool;
    std::function<void()> mJobLambda;
};

#endif //TranscriptionManager_h
