//
// Created by Damien Ronssin on 02.06.2024.
//

#ifndef TranscriptionManager_h
#define TranscriptionManager_h

#include <cstdint>

#include <JuceHeader.h>
#include "ActivityLog.h"
#include "BasicPitch.h"
#include "EngineCatalog.h"
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

    /** Why a take did not run on the engine it was asked to run on. */
    enum class EngineFallback {
        None = 0,
        /** QUARRY_SIDECAR_CMD is unset, so there is no out-of-process tier to reach at all. */
        SidecarNotConfigured,
        /** The sidecar process would not start, or has been given up on for this session. */
        SidecarStartFailed,
        /** The sidecar is up, but its interpreter cannot import that engine. */
        EngineNotInstalled,
        /** The sidecar took the request and failed it. */
        TranscribeFailed,
    };

    /**
     * Which engine was asked for and which one answered, for the current take.
     *
     * These differ more often than is comfortable -- an unset command line, a missing package, a
     * dead child -- and until this existed the difference was invisible: the take came back, the
     * notes looked plausible, and nothing said they had come from somewhere else. Reported in
     * TranscriptionSummary beside the rest of what describes a take.
     */
    struct EngineRun {
        bool hasRun = false;
        int requestedEngine = EngineCatalog::BuiltIn;
        int actualEngine = EngineCatalog::BuiltIn;
        EngineFallback fallback = EngineFallback::None;
    };

    EngineRun getEngineRun() const;

    /** What the sidecar tier can currently offer, as of the last time it was asked. */
    struct SidecarStatus {
        /** QUARRY_SIDECAR_CMD is set. Known without talking to anything. */
        bool configured = false;
        /** A start has been attempted, so engines/device/error mean something. */
        bool probed = false;
        /** Wire names from the ready line: what this interpreter can import. */
        juce::StringArray engines;
        /** Whether the ready line named its engines at all; see SidecarClient::hasEngineList.
            False leaves `engines` empty and meaningless, which is a different thing from a
            sidecar that answered with none, and the two must not be read alike. */
        bool enginesKnown = false;
        /** "cuda" or "cpu". */
        juce::String device;
        /** Why the last attempt failed, empty when it did not. */
        juce::String error;
    };

    SidecarStatus getSidecarStatus() const;

    /**
     * Ask the sidecar what it can do without transcribing anything, so the picker can grey out
     * engines before the first take rather than after the first failure. Returns immediately;
     * the work happens on its own single thread, because starting the child can take a minute
     * and loads a model's worth of memory, and doing it twice at once would load two.
     *
     * Its own thread rather than the transcription pool: that pool has one thread, so a probe
     * queued when the editor opens put the first take of the session behind a start that blocks
     * for up to SidecarClient::kReadyTimeoutMs, including takes on the built-in engine that
     * never go near the sidecar. mSidecarClientLock is what keeps the two off each other now.
     */
    void requestSidecarProbe();

    /** Any thread: the sidecar's stage/stderr lines and Quarry's own, in one feed the drawer polls. */
    quarry::ActivityLog& getActivityLog();

    /** What the current job (a take or a download) is doing right now, for the activity drawer's
     *  header line. text/fraction/cancellable are only meaningful while active is true. */
    struct Stage {
        juce::String text;
        double fraction = -1.0;
        bool active = false;
        /** True only while a sidecar transcription or a download is blocked in the client call
         *  that cancelCurrentJob() can actually interrupt -- not while the child is starting, and
         *  never for a built-in take, which has no cancellation point. */
        bool cancellable = false;
    };

    /** Any thread: copies the current stage out from under mStageLock. */
    Stage getCurrentStage() const;

    /**
     * Message thread; no-op unless a cancellable job is active right now. Marks mCancelRequested
     * and kills the sidecar child out from under whichever job is blocked in it; that job's own
     * transcribe()/download() call notices the broken pipe and returns false with "sidecar
     * terminated", which _tryTranscribeWithSidecar/_runDownload read as a cancel rather than a
     * failure (see their own comments) -- no failure count, no fallback to built-in, no pending
     * download file. The killed client is discarded, so the next job starts a fresh one.
     */
    void cancelCurrentJob();

    /**
     * Fetches inUrl through the sidecar (the only tier that can reach a URL) and, once the file is
     * on disk, drops it in exactly where a user's own drag-and-drop would have: see timerCallback,
     * which drains the finished download into mProcessor->getSourceAudioManager()->onFileDrop() on
     * the message thread. Message thread in, because it queues on mThreadPool and logs to
     * mActivityLog immediately (a same-thread caller wants "queued" to show up before this
     * returns); the fetch itself happens on that pool, same as a take.
     */
    void requestDownload(const juce::String& inUrl, const juce::File& inOutDir);

    /**
     * Called on the message thread when the recorded sidecar status has changed, so the MODEL
     * panel can rewrite its lines when the probe lands or a take moves the answer.
     *
     * A callback rather than a poll on the panel's side. The two things it needs to notice --
     * this, and the engine parameter moving -- both have somewhere to push from, and a 15 Hz
     * timer copying the whole status under a lock for the life of the editor was paying for an
     * answer that settles once and then does not move for hours.
     */
    std::function<void()> onSidecarStatusChanged;

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
     *
     * A cancel (cancelCurrentJob() killed the client mid-transcribe) is also a false return, but
     * a distinct one: mCancelRequested is set, nothing above is counted against the sidecar's
     * give-up budget, and the caller does not fall back to BasicPitch for it. See _runModel.
     */
    bool _tryTranscribeWithSidecar(const juce::File& inSourceFile,
                                   int inEngineIndex,
                                   std::vector<Notes::Event>& outNotes,
                                   std::vector<SidecarPedalEvent>& outPedal,
                                   EngineFallback& outFallback);

    /**
     * Starts mSidecarClient if it is not up, and records the result either way. Call with
     * mSidecarClientLock held.
     *
     * Does not count a failure against the give-up budget: that is the caller's to register,
     * because only the caller knows whether this attempt was a take. Counting it here let two
     * window opens -- which probe, and which nobody would call an attempt to transcribe
     * anything -- spend both strikes and retire the tier before the first take was ever run.
     */
    bool _ensureSidecarStarted(juce::String& outError);

    /** The body of requestSidecarProbe(), on the probe thread. */
    void _probeSidecar();

    /** Copies the ready line's answer into mSidecarStatus, under its lock. Null means "not up". */
    void _recordSidecarStatus(const SidecarClient* inClient, const juce::String& inError);

    /** Records one sidecar failure; the second in a row (with no success in between) gives up on
     * the sidecar tier for the rest of this session. */
    void _registerSidecarFailure();

    /** The current take's note events before post-processing, from whichever engine produced them. */
    const std::vector<Notes::Event>& _rawNoteEvents() const;

    /** Copies inText/inFraction/inActive into mCurrentStage under mStageLock. Leaves cancellable
     *  untouched -- see _setCancellable, its only mutator -- so a stage progress update arriving
     *  mid-transcribe (client->onStage) cannot flip the Cancel button off underneath the job. */
    void _setStage(const juce::String& inText, double inFraction, bool inActive);

    /** Flips mCurrentStage.cancellable under mStageLock. Set true just before the blocking
     *  transcribe()/download() call and false right after, on the job's own thread. */
    void _setCancellable(bool inCancellable);

    /** Body of one requestDownload() job, on mThreadPool. Starts mSidecarClient lazily (same helper
     *  _tryTranscribeWithSidecar uses), downloads, logs the result, and on success leaves the file
     *  in mPendingDownloadFile for timerCallback to drain. */
    void _runDownload(const juce::String& inUrl, const juce::File& inOutDir);

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
    // pay that once. Declared ahead of both pools so it is destroyed after they have finished
    // waiting on any in-flight job, the same ordering mBasicPitch already relies on.
    //
    // Two threads reach these now -- a take on mThreadPool and a probe on mProbePool -- so they
    // are held under a lock rather than by the pool being one thread wide. The lock is held for
    // the whole of a start or a transcribe, which is the point: those two must not overlap, and
    // a take that wants the sidecar should wait for the probe that is already starting it.
    mutable juce::CriticalSection mSidecarClientLock;
    std::unique_ptr<SidecarClient> mSidecarClient;
    int mSidecarFailureCount = 0;
    bool mSidecarUnavailable = false;

    // Guards only the mSidecarClient pointer itself (every assignment and reset of it), not the
    // call a job is mid-way through -- that is mSidecarClientLock above, held for a whole start-
    // or-transcribe, which cancelCurrentJob() (message thread) must never wait on. Taken around
    // cancel's read-and-kill() too, so the object it calls kill() on cannot be destroyed out from
    // under it; whichever of the two gets here first goes first, and kill() is bounded (at most
    // 2 s for its pump stop, see SidecarClient::kill()), so the loser just waits that long.
    mutable juce::CriticalSection mSidecarClientPointerLock;

    // Set by cancelCurrentJob() before it calls kill(); cleared at the start of every job (a take
    // on the sidecar, a take on the built-in engine, or a download). A false return from the
    // client call together with this set is a cancel, not a failure -- see
    // _tryTranscribeWithSidecar and _runDownload.
    std::atomic<bool> mCancelRequested {false};

    // Read on the message thread by the picker, written on the transcription thread where the
    // client lives. Copied out under the lock rather than handing out a pointer to the client,
    // which the transcription thread is free to destroy between any two of the reader's lines.
    mutable juce::CriticalSection mSidecarStatusLock;
    SidecarStatus mSidecarStatus;

    // Bumped by every write to mSidecarStatus, so the message-thread timer can tell that the
    // answer moved without copying the whole struct to compare it. Read against
    // mLastNotifiedSidecarStatusRevision, which only the timer touches.
    std::atomic<std::uint32_t> mSidecarStatusRevision {0};
    std::uint32_t mLastNotifiedSidecarStatusRevision = 0;

    // Four plain atomics rather than one locked struct: the reader is a paint routine, this is
    // the only writer, and a torn read here would at worst name the wrong engine for one frame.
    std::atomic<bool> mEngineRunHasRun {false};
    std::atomic<int> mEngineRunRequested {EngineCatalog::BuiltIn};
    std::atomic<int> mEngineRunActual {EngineCatalog::BuiltIn};
    std::atomic<int> mEngineRunFallback {0};

    std::atomic<bool> mShouldProbeSidecar {false};

    // Every line the sidecar and Quarry itself have produced, for the activity drawer. Its own
    // lock, separate from mSidecarStatusLock: that one guards a snapshot answer that changes
    // rarely, this is an append-only feed written from three different threads (a take, a probe,
    // and whichever thread onStage/onStderrLine land on) and polled independently of it.
    quarry::ActivityLog mActivityLog;

    // What the current job is doing, for the drawer's header line. Separate from mSidecarStatus:
    // that describes what the sidecar tier can do, this describes what is happening right now,
    // and a take on the built-in engine has a stage but no sidecar status to speak of.
    mutable juce::CriticalSection mStageLock;
    Stage mCurrentStage;

    // Set by _runDownload on success, drained by timerCallback into onFileDrop on the message
    // thread -- the same thread a UI drop uses. Not a callback: the editor that called
    // requestDownload can be destroyed and recreated by the host while this manager (owned by the
    // processor) lives on, and a callback capturing it would dangle.
    mutable juce::CriticalSection mPendingDownloadLock;
    juce::File mPendingDownloadFile;
    bool mHasPendingDownload = false;

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

    // The probe's own thread. It used to share mThreadPool, which is one thread wide, so a probe
    // fired when the editor opened held the first take of the session behind a start that can
    // block for two minutes -- even a built-in take, which has no interest in the sidecar at
    // all. They are kept off each other by mSidecarClientLock instead, which is the thing that
    // actually needed protecting.
    ThreadPool mProbePool;

    std::function<void()> mJobLambda;
    std::function<void()> mProbeLambda;
};

#endif //TranscriptionManager_h
