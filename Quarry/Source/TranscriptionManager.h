//
// Created by Damien Ronssin on 02.06.2024.
//

#ifndef TranscriptionManager_h
#define TranscriptionManager_h

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

    std::atomic<bool> mShouldRunNewTranscription = false;
    std::atomic<bool> mShouldUpdateTranscription = false;
    std::atomic<bool> mShouldUpdatePostProcessing = false;
    std::atomic<bool> mShouldRepaintPianoRoll = false;

    ThreadPool mThreadPool;
    std::function<void()> mJobLambda;
};

#endif //TranscriptionManager_h
