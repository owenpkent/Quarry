//
// Created by Damien Ronssin on 02.06.2024.
//

#ifndef TranscriptionManager_h
#define TranscriptionManager_h

#include <cstdint>

#include <JuceHeader.h>
#include "BasicPitch.h"
#include "NoteOptions.h"
#include "SidecarActivation.h"
#include "SidecarTypes.h"
#include "TimeQuantizeOptions.h"

// Forward-declared rather than #include "SidecarClient.h" here: that header brings in
// <windows.h> (guarded with NOMINMAX, but wingdi.h's own global ::Rectangle still leaks through
// it), and this header reaches, transitively, every piece of Quarry's UI. Kept an incomplete type
// in the header, with the include -- and the ~TranscriptionManager() definition that needs the
// complete type -- confined to TranscriptionManager.cpp instead.
class SidecarClient;

class QuarryAudioProcessor;
class QuarryMainView;
class QuarryEditor;

class TranscriptionManager
    : public Timer
    , public AudioProcessorValueTreeState::Listener
{
public:
    explicit TranscriptionManager(QuarryAudioProcessor* inProcessor);

    ~TranscriptionManager() override;

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

    /**
     * Sustain-pedal (CC64) events from the sidecar for the current take, in the same wall-clock
     * seconds as the raw note events. Always empty when the sidecar tier is off, when the current
     * take fell back to BasicPitch, or when the active sidecar engine does not report pedal.
     */
    const std::vector<SidecarPedalEvent>& getPedalEvents() const;

    void clear();

private:
    void _runModel();

    void _updateTranscription();

    void _updatePostProcessing();

    void _repaintPianoRoll();

    /**
     * Try the sidecar tier for one take: lazily starts mSidecarClient if needed, then sends one
     * transcribe request for inSourceFile. On any failure (start, transcribe, transport) this logs
     * via DBG, tears mSidecarClient down so the next attempt starts from scratch, and returns
     * false; the caller falls back to BasicPitch for that take. A well-formed {"ok":false,...} or
     * a transport failure are the only "malformed result" cases the protocol can produce -- both
     * already come back as false from SidecarClient::transcribe, so there is nothing further to
     * validate here.
     */
    bool _tryTranscribeWithSidecar(const juce::File& inSourceFile,
                                   std::vector<Notes::Event>& outNotes,
                                   std::vector<SidecarPedalEvent>& outPedal);

    /** Records one sidecar failure; the second in a row (with no success in between) gives up on
     * the sidecar tier for the rest of this session. */
    void _registerSidecarFailure();

    /** The current take's note events before post-processing, from whichever engine produced them. */
    const std::vector<Notes::Event>& _rawNoteEvents() const;

    QuarryAudioProcessor* mProcessor;

    BasicPitch mBasicPitch;
    NoteOptions mNoteOptions;
    TimeQuantizeOptions mTimeQuantizeOptions;

    // Resolved once from QUARRY_SIDECAR_CMD / QUARRY_SIDECAR_ENGINE at construction; see
    // SidecarActivation.h for what each variable means. Empty/unset QUARRY_SIDECAR_CMD leaves
    // mSidecarActivation.active false and every take runs through BasicPitch exactly as before.
    SidecarActivation mSidecarActivation;

    // Lazily started on first use and kept alive across takes for the life of this
    // TranscriptionManager (i.e. the life of the plugin/app instance), rather than relaunched per
    // take: model loads are the expensive part, so the whole point of a persistent process is to
    // pay that once. Declared ahead of mThreadPool so it is destroyed after mThreadPool has
    // finished waiting on any in-flight job, the same ordering mBasicPitch already relies on.
    std::unique_ptr<SidecarClient> mSidecarClient;
    int mSidecarFailureCount = 0;
    bool mSidecarUnavailable = false;

    // This take's sidecar results, valid only while mUsingSidecarForCurrentTake is true.
    std::vector<Notes::Event> mSidecarNoteEvents;
    std::vector<SidecarPedalEvent> mSidecarPedalEvents;
    bool mUsingSidecarForCurrentTake = false;

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
