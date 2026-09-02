//
// Created by Damien Ronssin on 02.06.2024.
//

#include <algorithm>

#include "TranscriptionManager.h"
#include "NoteVelocity.h"
#include "PluginProcessor.h"
#include "QuarryMainView.h"
#include "Components/Views/StageCaption.h"

// Deliberately last: SidecarClient.h pulls in <windows.h>, which drags along wingdi.h's global
// ::Rectangle and makes every unqualified juce::Rectangle<T> above ambiguous if seen afterwards.
// See the forward-declaration comment on SidecarClient in TranscriptionManager.h.
#include "SidecarClient.h"

TranscriptionManager::TranscriptionManager(QuarryAudioProcessor* inProcessor)
    : mProcessor(inProcessor)
    , mTimeQuantizeOptions(inProcessor)
    , mThreadPool(1)
    , mProbePool(1)
{
    // QUARRY_SIDECAR_CMD / QUARRY_SIDECAR_ENGINE -- see SidecarActivation.h. Read once here rather
    // than per take: they select a tier for the process's whole lifetime, not a per-take option.
    mSidecarActivation = resolveSidecarActivation(SystemStats::getEnvironmentVariable("QUARRY_SIDECAR_CMD", {}),
                                                  SystemStats::getEnvironmentVariable("QUARRY_SIDECAR_ENGINE", {}));

    mJobLambda = [this] { _runModel(); };
    mProbeLambda = [this] { _probeSidecar(); };

    {
        const ScopedLock lock(mSidecarStatusLock);
        mSidecarStatus.configured = mSidecarActivation.active;
    }

    // QUARRY_SIDECAR_ENGINE now names the engine to *start on*, not the engine to use forever.
    // The parameter is the source of truth from here, so a session restored a moment after this
    // runs overrides it, which is the right way round: the environment is a default belonging to
    // a machine and the session is a decision someone made about a piece of music. Only the
    // command line still has to come from the environment, because it is a path to a Python
    // interpreter and there is nowhere in the UI yet for a person to type one.
    if (mSidecarActivation.active) {
        // startingEngine, not indexForWireName. The two differ on exactly one input and it is the
        // common one: QUARRY_SIDECAR_ENGINE unset, which SidecarActivation defaults to "auto".
        // indexForWireName answers BuiltIn there, and seeding that would leave every take on
        // BasicPitch on a machine that had configured a sidecar and said nothing further about
        // it -- 0.775 onset F1 where it asked for 0.98, with no fallback recorded and so nothing
        // in the summary saying a substitution had happened at all.
        const auto index = EngineCatalog::startingEngine(mSidecarActivation.engine);

        auto* engine_param = mProcessor->getParams()[ParameterHelpers::EngineId];
        engine_param->setValueNotifyingHost(
            engine_param->getNormalisableRange().convertTo0to1(static_cast<float>(index)));
    }

    auto& apvts = mProcessor->getAPVTS();

    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::EngineId), this);

    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::NoteSensitivityId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::SplitSensitivityId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::MinimumNoteDurationId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::PitchBendModeId), this);

    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::EnableNoteQuantizationId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::KeyRootNoteId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::KeyTypeId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::KeySnapModeId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::MinMidiNoteId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::MaxMidiNoteId), this);

    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::EnableTimeQuantizationId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::TimeDivisionId), this);
    apvts.addParameterListener(ParameterHelpers::getIdStr(ParameterHelpers::QuantizationForceId), this);

    startTimerHz(30);
}

// Out-of-line so mSidecarClient (unique_ptr<SidecarClient>) is destroyed where SidecarClient is a
// complete type. Its own destructor shuts the child process down if still running.
TranscriptionManager::~TranscriptionManager() = default;

void TranscriptionManager::timerCallback()
{
    if (mTimeQuantizeOptions.checkInfoUpdated()) {
        mTimeQuantizeOptions.saveStateToValueTree(true);
    }

    if (mShouldProbeSidecar.exchange(false)) {
        mProbePool.addJob(mProbeLambda);
    }

    // The only message-thread pulse this class already owns, so the MODEL panel's two lines are
    // rewritten from here rather than from a second timer of its own polling for the same answer.
    const auto sidecar_status_revision = mSidecarStatusRevision.load();

    if (sidecar_status_revision != mLastNotifiedSidecarStatusRevision) {
        mLastNotifiedSidecarStatusRevision = sidecar_status_revision;

        if (onSidecarStatusChanged != nullptr) {
            onSidecarStatusChanged();
        }
    }

    // Drained here rather than handed to a callback: see mPendingDownloadFile's own comment. This
    // is the same thread a UI drop already lands on, so onFileDrop sees one calling thread either way.
    juce::File dropped_file;
    bool has_dropped_file = false;

    {
        const ScopedLock lock(mPendingDownloadLock);

        if (mHasPendingDownload) {
            dropped_file = mPendingDownloadFile;
            has_dropped_file = true;
            mHasPendingDownload = false;
        }
    }

    if (has_dropped_file) {
        if (dropped_file.existsAsFile()) {
            mProcessor->getSourceAudioManager()->onFileDrop(dropped_file);
        } else {
            mActivityLog.add(quarry::ActivityLine::Kind::Error,
                             "downloaded file missing: " + dropped_file.getFullPathName());
        }
    }

    if (mShouldRunNewTranscription) {
        launchTranscribeJob();
        _repaintPianoRoll();
    } else if (mShouldUpdateTranscription) {
        _updateTranscription();
        _repaintPianoRoll();
    } else if (mShouldUpdatePostProcessing) {
        _updatePostProcessing();
        _repaintPianoRoll();
    } else if (mShouldRepaintPianoRoll) {
        _repaintPianoRoll();
    }
}
void TranscriptionManager::prepareToPlay(double inSampleRate)
{
    mTimeQuantizeOptions.prepareToPlay(inSampleRate);
}

void TranscriptionManager::processBlock(int inNumSamples)
{
    mTimeQuantizeOptions.processBlock(inNumSamples);
}

void TranscriptionManager::setLaunchNewTranscription()
{
    mShouldRunNewTranscription = true;
    mShouldUpdateTranscription = false;
    mShouldUpdatePostProcessing = false;
}

void TranscriptionManager::parameterChanged(const String& parameterID, float newValue)
{
    if (mProcessor->getState() == PopulatedAudioAndMidiRegions) {
        if (parameterID == ParameterHelpers::getIdStr(ParameterHelpers::NoteSensitivityId)
            || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::SplitSensitivityId)
            || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::MinimumNoteDurationId)) {
            mProcessor->getAPVTS().getRawParameterValue(parameterID)->store(newValue);
            mShouldUpdateTranscription = true;

        } else if (parameterID == ParameterHelpers::getIdStr(ParameterHelpers::EnableNoteQuantizationId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::KeyRootNoteId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::KeyTypeId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::KeySnapModeId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::MinMidiNoteId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::MaxMidiNoteId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::EnableTimeQuantizationId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::TimeDivisionId)
                   || parameterID == ParameterHelpers::getIdStr(ParameterHelpers::QuantizationForceId)) {
            mProcessor->getAPVTS().getRawParameterValue(parameterID)->store(newValue);
            mShouldUpdatePostProcessing = true;
        } else if (parameterID == ParameterHelpers::getIdStr(ParameterHelpers::PitchBendModeId)) {
            mProcessor->getAPVTS().getRawParameterValue(parameterID)->store(newValue);
            mShouldRepaintPianoRoll = true;

        } else if (parameterID == ParameterHelpers::getIdStr(ParameterHelpers::EngineId)) {
            mProcessor->getAPVTS().getRawParameterValue(parameterID)->store(newValue);
            // The third bucket, and the only one that goes all the way back to the model. A
            // different engine is a different reading of the audio, not a different treatment of
            // the same notes, so neither of the other two paths can serve it: _updateTranscription
            // re-decodes what BasicPitch already heard, and _updatePostProcessing does not even
            // do that.
            setLaunchNewTranscription();
        }
    }
}

bool TranscriptionManager::_ensureSidecarStarted(juce::String& outError)
{
    if (mSidecarClient != nullptr && mSidecarClient->isRunning()) {
        // Re-recorded rather than returned from early, so a status left saying "unreachable" by
        // a failure that has since been recovered from cannot outlive it. Nothing else re-probes
        // once the editor's first probe has run, so without this the MODEL panel would grey all
        // seven rows and pin a stale error for the rest of the session, while every take went on
        // transcribing perfectly well on the engine the panel called unreachable.
        _recordSidecarStatus(mSidecarClient.get(), {});
        return true;
    }

    if (mSidecarClient != nullptr) {
        // Non-null but dead. This used to return true on the pointer alone, which handed the next
        // job a corpse: transcribe() rejects it with "sidecar is not running", and
        // _tryTranscribeWithSidecar cannot tell that from an engine that genuinely failed, so it
        // spent a strike. Two strikes retire the sidecar tier for the rest of the session -- the
        // whole seven-engine list gone, over a client nobody had asked whether it was alive.
        //
        // Starting a fresh one is what the caller wanted in the first place, and is what would
        // have happened had the pointer been cleared on whatever path left it like this.
        mActivityLog.add(quarry::ActivityLine::Kind::Quarry, "sidecar: the previous process is gone, starting another");

        const ScopedLock pointer_lock(mSidecarClientPointerLock);
        mSidecarClient.reset();
    }

    mActivityLog.add(quarry::ActivityLine::Kind::Quarry, "sidecar: starting " + mSidecarActivation.commandLine);

    auto client = std::make_unique<SidecarClient>(mSidecarActivation.commandLine);

    // Wired before start(), not after: a slow model load reports stage events while start() itself
    // is still blocked waiting on "ready", and those are exactly the lines someone watching the
    // drawer wants during the wait.
    client->onStage = [this](const SidecarStage& inStage) {
        // The drawer gets the sidecar's whole sentence; the strip gets the few words it has room
        // for. See StageCaption.h -- the strip's caption area is 169 px, and handing it the
        // sentence truncated it to the half that named no stage at all.
        mActivityLog.add(quarry::ActivityLine::Kind::Stage, inStage.text);
        _setStage(quarry::stageCaption(inStage.stage, inStage.text), inStage.fraction, true);
    };

    client->onStderrLine = [this](const juce::String& inLine) {
        mActivityLog.add(quarry::ActivityLine::Kind::Stderr, inLine);
    };

    if (!client->start(outError)) {
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "sidecar: " + outError);
        _recordSidecarStatus(nullptr, outError);

        // The stage the onStage above may already have set, taken back down. start() deliberately
        // forwards stage events it sees before the "ready" line, which is the whole reason that
        // callback is wired before this call -- so a start that then fails can leave the strip
        // showing a stage with nothing behind it. Nothing would ever clear it: the job that would
        // have is the one that just failed to begin, and the strip's CANCEL only shows for a
        // cancellable stage. The bar swept, for the rest of the session.
        _setStage({}, -1.0, false);
        return false;
    }

    mActivityLog.add(quarry::ActivityLine::Kind::Quarry,
                     "sidecar ready: protocol " + juce::String(client->getProtocolVersion()) + ", "
                         + client->getDevice() + ", engines "
                         + (client->hasEngineList() ? client->getAvailableEngines().joinIntoString(", ")
                                                     : "unknown"));

    {
        const ScopedLock pointer_lock(mSidecarClientPointerLock);
        mSidecarClient = std::move(client);
    }

    _recordSidecarStatus(mSidecarClient.get(), {});

    return true;
}

void TranscriptionManager::_probeSidecar()
{
    // Try rather than wait. A take holding the client is starting or using the very thing this
    // was going to ask about, and it records the answer itself either way, so there is nothing
    // here to learn by queueing behind a transcription that can run for a minute -- and a probe
    // thread parked on a lock for that long is a thread the destructor then has to wait out.
    const ScopedTryLock lock(mSidecarClientLock);

    if (!lock.isLocked()) {
        return;
    }

    if (mSidecarUnavailable) {
        _recordSidecarStatus(nullptr, "gave up on the sidecar after repeated failures");
        return;
    }

    juce::String error;

    // No _registerSidecarFailure here. Opening the plugin window is not an attempt to transcribe
    // anything, and spending a strike on it meant two window opens could retire the tier before
    // a take had ever asked for it -- taking with them the one from-scratch retry docs/SIDECAR.md
    // promises. The budget is for takes; this only reports.
    if (!_ensureSidecarStarted(error)) {
        DBG("Sidecar probe failed: " << error);
    }

    // And on the way out, whether it worked or not. A probe runs no job, so any stage the model
    // load reported while start() was blocked belongs to nothing: opening the plugin window on a
    // cold sidecar left the strip sweeping under a caption like "loading the model" that no take
    // was ever going to finish and no CANCEL was ever going to clear.
    _setStage({}, -1.0, false);
}

void TranscriptionManager::_recordSidecarStatus(const SidecarClient* inClient, const juce::String& inError)
{
    const ScopedLock lock(mSidecarStatusLock);

    mSidecarStatus.configured = mSidecarActivation.active;
    mSidecarStatus.probed = true;
    mSidecarStatus.error = inError;

    if (inClient != nullptr) {
        mSidecarStatus.engines = inClient->getAvailableEngines();
        mSidecarStatus.enginesKnown = inClient->hasEngineList();
        mSidecarStatus.device = inClient->getDevice();
    } else {
        mSidecarStatus.engines.clear();
        mSidecarStatus.enginesKnown = false;
        mSidecarStatus.device.clear();
    }

    // Outside the lock's concern but inside its scope, so a reader that sees the new revision is
    // guaranteed to see the fields it counts for. The message-thread timer watches this and calls
    // onSidecarStatusChanged; see requestSidecarProbe.
    mSidecarStatusRevision.fetch_add(1);
}

bool TranscriptionManager::_tryTranscribeWithSidecar(const juce::File& inSourceFile,
                                                      int inEngineIndex,
                                                      std::vector<Notes::Event>& outNotes,
                                                      std::vector<SidecarPedalEvent>& outPedal,
                                                      EngineFallback& outFallback)
{
    if (!mSidecarActivation.active) {
        outFallback = EngineFallback::SidecarNotConfigured;
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "sidecar not configured, falling back to built-in");
        return false;
    }

    // Held for the whole take, including the transcribe itself: a probe must not start a second
    // child underneath one, and a probe already in flight is starting the very client this take
    // is about to want, so waiting for it is the right thing rather than the slow thing.
    const ScopedLock lock(mSidecarClientLock);

    if (mSidecarUnavailable) {
        outFallback = EngineFallback::SidecarStartFailed;
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "sidecar unavailable this session, falling back to built-in");
        return false;
    }

    juce::String error;

    if (!_ensureSidecarStarted(error)) {
        // _ensureSidecarStarted has already logged the failure itself (it is the one place that
        // starts the child, whether this take triggered it or an earlier probe did).
        DBG("Sidecar failed to start (" << mSidecarActivation.commandLine << "): " << error);
        _registerSidecarFailure();
        outFallback = EngineFallback::SidecarStartFailed;
        return false;
    }

    const juce::String wire_name(EngineCatalog::get(inEngineIndex).wireName);

    // The ready line already said which engines this interpreter can import, so asking for one
    // that is not on it is a round trip to be told what we were told at startup. Worse, a failed
    // transcribe counts against the session-wide give-up below, which would let one missing
    // package take the whole tier down with it.
    //
    // Only when it said, though. A ready line with no "engines" field parses to the same empty
    // list as a sidecar with nothing installed, and refusing on that would turn every older or
    // third-party serve process into seven engines that are all "not installed" -- a request
    // this used to send, and that used to be answered. Silence means send it and find out, which
    // is also what the picker does with the same unknown.
    if (mSidecarClient->hasEngineList() && !mSidecarClient->getAvailableEngines().contains(wire_name)) {
        DBG("Sidecar has no engine named " << wire_name);
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "sidecar: engine not installed: " + wire_name);
        outFallback = EngineFallback::EngineNotInstalled;
        return false;
    }

    std::vector<SidecarNote> sidecar_notes;

    // cancellable only for the duration of the blocking call itself: see cancelCurrentJob() and
    // Stage::cancellable's own comments for why start() above is not included.
    _setCancellable(true);
    const bool transcribed = mSidecarClient->transcribe(inSourceFile, wire_name, sidecar_notes, outPedal, error);
    _setCancellable(false);

    // Read after _setCancellable(false), and that order is the point. cancelCurrentJob holds
    // mStageLock across its own check and its kill(), so by the time this line runs a cancel has
    // either already happened -- and set this -- or has found cancellable clear and done nothing.
    // There is no third answer, which is what makes the two threads agree.
    //
    // A cancel that got in counts even when transcribe() came back true: the response and the
    // kill() can genuinely cross, and the child is dead either way. Honouring the result instead
    // would keep notes from a take the person asked to stop, and leave the killed client in place.
    const bool cancelled = mCancelRequested.load();

    if (!transcribed || cancelled) {
        // Discarded either way: a killed client is done, and so is one that failed outright --
        // both leave the next job's _ensureSidecarStarted starting fresh.
        {
            const ScopedLock pointer_lock(mSidecarClientPointerLock);
            mSidecarClient.reset();
        }

        if (cancelled) {
            mActivityLog.add(quarry::ActivityLine::Kind::Quarry, "cancelled");
            return false;
        }

        DBG("Sidecar transcription failed: " << error);
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "sidecar transcription failed: " + error);
        _recordSidecarStatus(nullptr, error);
        _registerSidecarFailure();
        outFallback = EngineFallback::TranscribeFailed;
        return false;
    }

    // Real per-note velocity is uniform across a response by protocol (kong/transkun: every note;
    // muscriptor: none), so any one note reporting a real velocity is enough to say this take has
    // sidecar-measured dynamics and NoteVelocity below should be skipped for it entirely.
    const bool has_measured_velocity =
        std::any_of(sidecar_notes.begin(), sidecar_notes.end(), [](const SidecarNote& n) { return n.velocity >= 0; });

    outNotes = toNotesEvents(sidecar_notes);

    if (!has_measured_velocity) {
        // muscriptor: no velocity in the response. This take never runs BasicPitch, so there is no
        // CQT for NoteVelocity to measure from -- but it is still the right stage to run: an
        // unprepared NoteVelocity gives every note its own "could not measure" fallback (a flat,
        // audible velocity) rather than leaving them at Event's silent-looking 0.0 default. Same
        // class, same behaviour a BasicPitch take gets when its own table is empty.
        NoteVelocity note_velocity;
        note_velocity.apply(outNotes);
    }

    mSidecarFailureCount = 0;
    outFallback = EngineFallback::None;

    return true;
}

void TranscriptionManager::_registerSidecarFailure()
{
    mSidecarFailureCount++;

    if (mSidecarFailureCount >= 2) {
        mSidecarUnavailable = true;
        DBG("Sidecar unavailable for the rest of this session after repeated failures.");
        mActivityLog.add(quarry::ActivityLine::Kind::Error,
                         "sidecar unavailable for the rest of this session after repeated failures");
    }
}

const std::vector<Notes::Event>& TranscriptionManager::_rawNoteEvents() const
{
    return mUsingSidecarForCurrentTake ? mSidecarNoteEvents : mBasicPitch.getNoteEvents();
}

void TranscriptionManager::_runModel()
{
    const auto job_start_ms = juce::Time::getMillisecondCounterHiRes();

    mUsingSidecarForCurrentTake = false;
    mCancelRequested.store(false);

    // getChoiceIndex, not a cast: the picker and the host's own readout both round, and
    // truncating here would transcribe with one engine while every readable surface named the
    // one above it. See ParameterHelpers::getChoiceIndex.
    const int requested_engine =
        jlimit(0,
               EngineCatalog::NumEngines - 1,
               ParameterHelpers::getChoiceIndex(mProcessor->getParams()[ParameterHelpers::EngineId]));

    const double take_seconds = mProcessor->getSourceAudioManager()->getNumSamplesDownAcquired()
                                 / static_cast<double>(AUDIO_SAMPLE_RATE);

    mActivityLog.add(quarry::ActivityLine::Kind::Quarry,
                     "job start: " + juce::String(take_seconds, 1) + "s take, engine "
                         + juce::String(EngineCatalog::get(requested_engine).displayName).toLowerCase());

    // mSidecarActivation.active, not just isSidecar: a sidecar engine picked with no sidecar
    // configured falls back to the built-in model a few lines below without ever waiting for a
    // child, so saying "waiting for sidecar" here names a wait that never happens. Read on this
    // thread exactly as _tryTranscribeWithSidecar reads it.
    const bool will_use_sidecar = EngineCatalog::isSidecar(requested_engine) && mSidecarActivation.active;

    _setStage(will_use_sidecar ? "waiting for sidecar" : "transcribing with built-in", -1.0, true);

    auto fallback = EngineFallback::None;

    if (EngineCatalog::isSidecar(requested_engine)) {
        // The best audio available, not the 22.05 kHz mono wav BasicPitch reads: the original file
        // dropped by the user, or the native-sample-rate wav a recording was captured to. Both
        // outlive the take (see SourceAudioManager::getSourceFile) so they are guaranteed to exist
        // here, on this background thread, once launchTranscribeJob has queued this job.
        const auto source_file = mProcessor->getSourceAudioManager()->getSourceFile();

        std::vector<Notes::Event> sidecar_notes;
        std::vector<SidecarPedalEvent> sidecar_pedal;

        if (_tryTranscribeWithSidecar(source_file, requested_engine, sidecar_notes, sidecar_pedal, fallback)) {
            mSidecarNoteEvents = std::move(sidecar_notes);
            mSidecarPedalEvents = std::move(sidecar_pedal);
            mUsingSidecarForCurrentTake = true;
        } else if (mCancelRequested.load()) {
            // Cancelled mid-flight, not failed: no built-in fallback and no post-processing,
            // because there is nothing new to post-process. The take already had audio, and --
            // if this job was a re-transcribe rather than the first one -- notes from before it
            // started; neither is touched, so PopulatedAudioAndMidiRegions (the state a finished
            // take already sits in) is where this lands, not Processing (which would leave the
            // transport disabled over notes that are still perfectly good) or Empty (which would
            // throw away notes this job never came near, and disable playback of audio that is
            // sitting right there either way).
            //
            // That holds for the first transcribe of a dropped file too, where clear() emptied
            // the notes when the file arrived and this job never replaced them: the audio is real
            // and playable, so the state is still the right one. What is not right is what the
            // state used to imply on its own -- see QuarryMainView, where the MIDI drag is now
            // offered on there being notes rather than on having reached this state, because it
            // was handing out an empty .mid for exactly this cancel.
            //
            // The revision bump goes with it. Without one, a piano roll that had notes before a
            // re-transcribe has no reason to repaint, and goes on drawing them.
            _setStage({}, -1.0, false);
            mRawNoteEventsRevision.fetch_add(1);
            mProcessor->setStateToPopulatedAudioAndMidiRegions();
            return;
        }
    }

    // Written before the decode rather than after it, so a reader catching this take mid-flight
    // sees the engine producing it and not the one that produced the last.
    mEngineRunRequested.store(requested_engine);
    mEngineRunActual.store(mUsingSidecarForCurrentTake ? requested_engine : EngineCatalog::BuiltIn);
    mEngineRunFallback.store(static_cast<int>(mUsingSidecarForCurrentTake ? EngineFallback::None : fallback));
    mEngineRunHasRun.store(true);

    if (!mUsingSidecarForCurrentTake) {
        mSidecarPedalEvents.clear();

        mBasicPitch.setParameters(mProcessor->getParameterValue(ParameterHelpers::NoteSensitivityId),
                                  mProcessor->getParameterValue(ParameterHelpers::SplitSensitivityId),
                                  mProcessor->getParameterValue(ParameterHelpers::MinimumNoteDurationId));

        const auto model_start_ms = juce::Time::getMillisecondCounterHiRes();

        mBasicPitch.transcribeToMIDI(
            mProcessor->getSourceAudioManager()->getDownsampledSourceAudioForTranscription().getWritePointer(0),
            mProcessor->getSourceAudioManager()->getNumSamplesDownAcquired());

        const auto model_ms = juce::Time::getMillisecondCounterHiRes() - model_start_ms;

        mActivityLog.add(quarry::ActivityLine::Kind::Quarry,
                         "built-in model: " + juce::String(static_cast<int>(mBasicPitch.getNoteEvents().size()))
                             + " notes in " + juce::String(model_ms, 0) + " ms");
    }

    mRawNoteEventsRevision.fetch_add(1);

    const bool snap_enabled = mProcessor->getParameterValue(ParameterHelpers::EnableNoteQuantizationId) > 0.5f;

    mNoteOptions.setParameters(
        snap_enabled,
        static_cast<NoteUtils::RootNote>(mProcessor->getParameterValue(ParameterHelpers::KeyRootNoteId)),
        static_cast<NoteUtils::ScaleType>(mProcessor->getParameterValue(ParameterHelpers::KeyTypeId)),
        static_cast<NoteUtils::SnapMode>(mProcessor->getParameterValue(ParameterHelpers::KeySnapModeId)),
        static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::MinMidiNoteId)),
        static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::MaxMidiNoteId)));

    mActivityLog.add(quarry::ActivityLine::Kind::Quarry, snap_enabled ? "scale snap: on" : "scale snap: off");

    auto post_processed_notes = mNoteOptions.process(_rawNoteEvents());

    const bool time_quantize_enabled =
        mProcessor->getParameterValue(ParameterHelpers::EnableTimeQuantizationId) > 0.5f;

    mTimeQuantizeOptions.setParameters(
        time_quantize_enabled,
        static_cast<TimeQuantizeUtils::TimeDivisions>(mProcessor->getParameterValue(ParameterHelpers::TimeDivisionId)),
        mProcessor->getParameterValue(ParameterHelpers::QuantizationForceId));

    mActivityLog.add(quarry::ActivityLine::Kind::Quarry,
                     time_quantize_enabled ? "time quantize: on" : "time quantize: off");

    mPostProcessedNotes = mTimeQuantizeOptions.quantize(post_processed_notes);

    Notes::dropOverlappingPitchBends(mPostProcessedNotes);
    Notes::mergeOverlappingNotesWithSamePitch(mPostProcessedNotes);

    // For the synth
    auto single_events = SynthController::buildMidiEventsVector(mPostProcessedNotes);
    mProcessor->getPlayer()->getSynthController()->setNewMidiEventsVectorToUse(single_events);

    mProcessor->setStateToPopulatedAudioAndMidiRegions();

    const auto job_elapsed_s = (juce::Time::getMillisecondCounterHiRes() - job_start_ms) / 1000.0;
    mActivityLog.add(quarry::ActivityLine::Kind::Quarry, "job done in " + juce::String(job_elapsed_s, 2) + " s");

    _setStage({}, -1.0, false);
}

void TranscriptionManager::_updateTranscription()
{
    jassert(mProcessor->getState() == PopulatedAudioAndMidiRegions);

    if (mProcessor->getState() == PopulatedAudioAndMidiRegions) {
        // NoteSensitivity/SplitSensitivity/MinimumNoteDuration are BasicPitch decoder knobs with no
        // sidecar equivalent: a sidecar take's notes are already final, so there is nothing to
        // re-decode. Skip straight to post-processing (key snap, min/max note, quantization),
        // which does apply to a sidecar take just as it does to a BasicPitch one.
        if (!mUsingSidecarForCurrentTake) {
            mBasicPitch.setParameters(mProcessor->getParameterValue(ParameterHelpers::NoteSensitivityId),
                                      mProcessor->getParameterValue(ParameterHelpers::SplitSensitivityId),
                                      mProcessor->getParameterValue(ParameterHelpers::MinimumNoteDurationId));

            mBasicPitch.updateMIDI();
            mRawNoteEventsRevision.fetch_add(1);
        }

        _updatePostProcessing();
    }

    mShouldUpdateTranscription = false;
    mShouldUpdatePostProcessing = false;
}

void TranscriptionManager::_updatePostProcessing()
{
    jassert(mProcessor->getState() == PopulatedAudioAndMidiRegions);

    if (mProcessor->getState() == PopulatedAudioAndMidiRegions) {
        mNoteOptions.setParameters(
            mProcessor->getParameterValue(ParameterHelpers::EnableNoteQuantizationId) > 0.5f,
            static_cast<NoteUtils::RootNote>(mProcessor->getParameterValue(ParameterHelpers::KeyRootNoteId)),
            static_cast<NoteUtils::ScaleType>(mProcessor->getParameterValue(ParameterHelpers::KeyTypeId)),
            static_cast<NoteUtils::SnapMode>(mProcessor->getParameterValue(ParameterHelpers::KeySnapModeId)),
            static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::MinMidiNoteId)),
            static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::MaxMidiNoteId)));

        // TODO: Make this vector a member to avoid reallocating every time
        auto post_processed_notes = mNoteOptions.process(_rawNoteEvents());

        mTimeQuantizeOptions.setParameters(mProcessor->getParameterValue(ParameterHelpers::EnableTimeQuantizationId)
                                               > 0.5f,
                                           static_cast<TimeQuantizeUtils::TimeDivisions>(
                                               mProcessor->getParameterValue(ParameterHelpers::TimeDivisionId)),
                                           mProcessor->getParameterValue(ParameterHelpers::QuantizationForceId));

        // TODO: Pass mPostProcessedNotes as reference
        mPostProcessedNotes = mTimeQuantizeOptions.quantize(post_processed_notes);

        Notes::dropOverlappingPitchBends(mPostProcessedNotes);
        Notes::mergeOverlappingNotesWithSamePitch(mPostProcessedNotes);

        // For the synth
        auto single_events = SynthController::buildMidiEventsVector(mPostProcessedNotes);
        mProcessor->getPlayer()->getSynthController()->setNewMidiEventsVectorToUse(single_events);
    }

    mShouldUpdatePostProcessing = false;
}

TranscriptionManager::EngineRun TranscriptionManager::getEngineRun() const
{
    EngineRun run;

    run.hasRun = mEngineRunHasRun.load();
    run.requestedEngine = mEngineRunRequested.load();
    run.actualEngine = mEngineRunActual.load();
    run.fallback = static_cast<EngineFallback>(mEngineRunFallback.load());

    return run;
}

TranscriptionManager::SidecarStatus TranscriptionManager::getSidecarStatus() const
{
    const ScopedLock lock(mSidecarStatusLock);
    return mSidecarStatus;
}

void TranscriptionManager::requestSidecarProbe()
{
    if (!mSidecarActivation.active) {
        // Nothing to ask, and the answer is already known. Recorded as probed anyway, so the
        // picker can say "not configured" rather than sitting on "checking" for a process that
        // is never going to start.
        _recordSidecarStatus(nullptr, "QUARRY_SIDECAR_CMD is not set");
        return;
    }

    mShouldProbeSidecar = true;
}

quarry::ActivityLog& TranscriptionManager::getActivityLog()
{
    return mActivityLog;
}

TranscriptionManager::Stage TranscriptionManager::getCurrentStage() const
{
    const ScopedLock lock(mStageLock);
    return mCurrentStage;
}

void TranscriptionManager::_setStage(const juce::String& inText, double inFraction, bool inActive)
{
    const ScopedLock lock(mStageLock);

    mCurrentStage.text = inText;
    mCurrentStage.fraction = inFraction;
    mCurrentStage.active = inActive;
}

void TranscriptionManager::_setCancellable(bool inCancellable)
{
    const ScopedLock lock(mStageLock);
    mCurrentStage.cancellable = inCancellable;
}

void TranscriptionManager::cancelCurrentJob()
{
    jassert(MessageManager::getInstance()->isThisTheMessageThread());

    // The check, the flag and the kill all under mStageLock, which is the lock _setCancellable
    // writes through. Reading cancellable and then dropping the lock -- as this did -- left a gap
    // exactly the width of two statements on the job thread: transcribe() returns true, and the
    // _setCancellable(false) on the very next line has not run yet. A click landing there found
    // cancellable still set and terminated a child that had already answered. The take then
    // completed normally off the response it had, so nothing looked wrong, while a loaded model
    // and its VRAM were thrown away and a dead client was left in mSidecarClient for the next
    // take to trip over.
    //
    // Held across all three, the job thread's own _setCancellable(false) either gets the lock
    // first -- and this returns having killed nothing -- or waits behind this and then reads the
    // mCancelRequested set below. Either way the two agree on whether this take was cancelled.
    const ScopedLock stage_lock(mStageLock);

    if (!mCurrentStage.cancellable)
        return;

    mCancelRequested.store(true);

    // See mSidecarClientPointerLock's own comment: this is the read-and-kill() side of it. Not
    // mSidecarClientLock -- the job holds that for the whole call, which is exactly what a cancel
    // has to reach past rather than wait behind.
    //
    // Taken under mStageLock, and only ever in that order: nothing anywhere takes this one first
    // and then reaches for the stage.
    const ScopedLock lock(mSidecarClientPointerLock);

    if (mSidecarClient != nullptr)
        mSidecarClient->kill();
}

void TranscriptionManager::requestDownload(const juce::String& inUrl, const juce::File& inOutDir)
{
    jassert(MessageManager::getInstance()->isThisTheMessageThread());

    if (!mSidecarActivation.active) {
        mActivityLog.add(quarry::ActivityLine::Kind::Error,
                         "download needs the sidecar; set QUARRY_SIDECAR_CMD (docs/SIDECAR.md)");
        return;
    }

    mActivityLog.add(quarry::ActivityLine::Kind::Quarry, "download: " + inUrl);

    // mThreadPool, not a pool of its own: the client allows one request in flight, so a download
    // and a take have to queue behind each other regardless, and this is the queue that already does it.
    mThreadPool.addJob([this, inUrl, inOutDir] { _runDownload(inUrl, inOutDir); });
}

void TranscriptionManager::_runDownload(const juce::String& inUrl, const juce::File& inOutDir)
{
    // Held for the whole download, same as a take: see _tryTranscribeWithSidecar's own lock comment.
    const ScopedLock lock(mSidecarClientLock);

    mCancelRequested.store(false);

    if (mSidecarUnavailable) {
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "download failed: sidecar unavailable this session");
        return;
    }

    juce::String start_error;

    if (!_ensureSidecarStarted(start_error)) {
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "download failed: " + start_error);
        return;
    }

    _setStage("downloading", -1.0, true);

    const auto start_ms = juce::Time::getMillisecondCounterHiRes();

    juce::File out_file;
    juce::String title;
    juce::String error;

    _setCancellable(true);
    const bool ok = mSidecarClient->download(inUrl, inOutDir, out_file, title, error);
    _setCancellable(false);

    if (ok) {
        const auto elapsed_s = (juce::Time::getMillisecondCounterHiRes() - start_ms) / 1000.0;
        mActivityLog.add(quarry::ActivityLine::Kind::Quarry,
                         "downloaded: " + title + " -> " + out_file.getFullPathName() + " in "
                             + juce::String(elapsed_s, 1) + " s");
    } else if (mCancelRequested.load()) {
        mActivityLog.add(quarry::ActivityLine::Kind::Quarry, "download cancelled");

        // Discarded same as a cancelled take: see _tryTranscribeWithSidecar's own comment.
        const ScopedLock pointer_lock(mSidecarClientPointerLock);
        mSidecarClient.reset();
    } else {
        mActivityLog.add(quarry::ActivityLine::Kind::Error, "download failed: " + error);

        // The same treatment _tryTranscribeWithSidecar gives the same class of failure, which
        // this branch used to skip entirely. A download fails this way when the child died under
        // it -- ffmpeg out of memory, yt-dlp segfaulting, something outside killing the process --
        // and the client left behind is non-null and dead. The MODEL panel went on advertising all
        // seven engines off a status nobody had corrected, and the next take inherited the corpse.
        {
            const ScopedLock pointer_lock(mSidecarClientPointerLock);
            mSidecarClient.reset();
        }

        _recordSidecarStatus(nullptr, error);
        _registerSidecarFailure();
    }

    _setStage({}, -1.0, false);

    if (ok) {
        const ScopedLock pending_lock(mPendingDownloadLock);
        mPendingDownloadFile = out_file;
        mHasPendingDownload = true;
    }
}

const std::vector<Notes::Event>& TranscriptionManager::getNoteEventVector() const
{
    return mPostProcessedNotes;
}

const std::vector<Notes::Event>& TranscriptionManager::getRawNoteEventVector() const
{
    return _rawNoteEvents();
}

std::uint32_t TranscriptionManager::getRawNoteEventsRevision() const
{
    return mRawNoteEventsRevision.load();
}

TimeQuantizeOptions& TranscriptionManager::getTimeQuantizeOptions()
{
    return mTimeQuantizeOptions;
}

const std::vector<SidecarPedalEvent>& TranscriptionManager::getPedalEvents() const
{
    return mSidecarPedalEvents;
}

void TranscriptionManager::clear()
{
    mBasicPitch.reset();
    mRawNoteEventsRevision.fetch_add(1);
    mShouldRunNewTranscription = false;
    mShouldUpdateTranscription = false;
    mShouldUpdatePostProcessing = false;
    mPostProcessedNotes.clear();
    mTimeQuantizeOptions.clear();

    // Per-take sidecar results only; mSidecarClient/mSidecarUnavailable/mSidecarFailureCount are
    // process-lifetime state and outlive any one take on purpose.
    mSidecarNoteEvents.clear();
    mSidecarPedalEvents.clear();
    mUsingSidecarForCurrentTake = false;

    // Which engine read the take goes with the take. The summary already stops drawing it once
    // there is no reading, but leaving the record standing would have the next reader of
    // getEngineRun() told about a take that no longer exists.
    mEngineRunHasRun.store(false);
    mEngineRunFallback.store(static_cast<int>(EngineFallback::None));
}

void TranscriptionManager::launchTranscribeJob()
{
    jassert(MessageManager::getInstance()->isThisTheMessageThread());
    mProcessor->setStateToProcessing();

    // Have at least one second to transcribe
    if (mProcessor->getSourceAudioManager()->getNumSamplesDownAcquired() >= 1 * AUDIO_SAMPLE_RATE) {
        mThreadPool.addJob(mJobLambda);
    } else {
        mProcessor->clear();
    }

    mShouldRunNewTranscription = false;
    mShouldUpdateTranscription = false;
    mShouldUpdatePostProcessing = false;
}

void TranscriptionManager::_repaintPianoRoll()
{
    auto* main_view = mProcessor->getQuarryMainView();

    if (main_view) {
        main_view->repaintPianoRoll();
    }

    mShouldRepaintPianoRoll = false;
}
