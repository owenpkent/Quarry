//
// Created by Damien Ronssin on 02.06.2024.
//

#include <algorithm>

#include "TranscriptionManager.h"
#include "NoteVelocity.h"
#include "PluginProcessor.h"
#include "QuarryMainView.h"

// Deliberately last: SidecarClient.h pulls in <windows.h>, which drags along wingdi.h's global
// ::Rectangle and makes every unqualified juce::Rectangle<T> above ambiguous if seen afterwards.
// See the forward-declaration comment on SidecarClient in TranscriptionManager.h.
#include "SidecarClient.h"

TranscriptionManager::TranscriptionManager(QuarryAudioProcessor* inProcessor)
    : mProcessor(inProcessor)
    , mTimeQuantizeOptions(inProcessor)
    , mThreadPool(1)
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
        const auto index = EngineCatalog::indexForWireName(mSidecarActivation.engine);

        if (index != EngineCatalog::BuiltIn) {
            auto* engine_param = mProcessor->getParams()[ParameterHelpers::EngineId];
            engine_param->setValueNotifyingHost(
                engine_param->getNormalisableRange().convertTo0to1(static_cast<float>(index)));
        }
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
        mThreadPool.addJob(mProbeLambda);
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
    if (mSidecarClient != nullptr) {
        return true;
    }

    auto client = std::make_unique<SidecarClient>(mSidecarActivation.commandLine);

    if (!client->start(outError)) {
        _registerSidecarFailure();
        _recordSidecarStatus(nullptr, outError);
        return false;
    }

    mSidecarClient = std::move(client);
    _recordSidecarStatus(mSidecarClient.get(), {});

    return true;
}

void TranscriptionManager::_probeSidecar()
{
    if (mSidecarUnavailable) {
        _recordSidecarStatus(nullptr, "gave up on the sidecar after repeated failures");
        return;
    }

    juce::String error;

    if (!_ensureSidecarStarted(error)) {
        DBG("Sidecar probe failed: " << error);
    }
}

void TranscriptionManager::_recordSidecarStatus(const SidecarClient* inClient, const juce::String& inError)
{
    const ScopedLock lock(mSidecarStatusLock);

    mSidecarStatus.configured = mSidecarActivation.active;
    mSidecarStatus.probed = true;
    mSidecarStatus.error = inError;

    if (inClient != nullptr) {
        mSidecarStatus.engines = inClient->getAvailableEngines();
        mSidecarStatus.device = inClient->getDevice();
    } else {
        mSidecarStatus.engines.clear();
        mSidecarStatus.device.clear();
    }
}

bool TranscriptionManager::_tryTranscribeWithSidecar(const juce::File& inSourceFile,
                                                      int inEngineIndex,
                                                      std::vector<Notes::Event>& outNotes,
                                                      std::vector<SidecarPedalEvent>& outPedal,
                                                      EngineFallback& outFallback)
{
    if (!mSidecarActivation.active) {
        outFallback = EngineFallback::SidecarNotConfigured;
        return false;
    }

    if (mSidecarUnavailable) {
        outFallback = EngineFallback::SidecarStartFailed;
        return false;
    }

    juce::String error;

    if (!_ensureSidecarStarted(error)) {
        DBG("Sidecar failed to start (" << mSidecarActivation.commandLine << "): " << error);
        outFallback = EngineFallback::SidecarStartFailed;
        return false;
    }

    const juce::String wire_name(EngineCatalog::get(inEngineIndex).wireName);

    // The ready line already said which engines this interpreter can import, so asking for one
    // that is not on it is a round trip to be told what we were told at startup. Worse, a failed
    // transcribe counts against the session-wide give-up below, which would let one missing
    // package take the whole tier down with it.
    if (!mSidecarClient->getAvailableEngines().contains(wire_name)) {
        DBG("Sidecar has no engine named " << wire_name);
        outFallback = EngineFallback::EngineNotInstalled;
        return false;
    }

    std::vector<SidecarNote> sidecar_notes;

    if (!mSidecarClient->transcribe(inSourceFile, wire_name, sidecar_notes, outPedal, error)) {
        DBG("Sidecar transcription failed: " << error);
        mSidecarClient.reset();
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
    }
}

const std::vector<Notes::Event>& TranscriptionManager::_rawNoteEvents() const
{
    return mUsingSidecarForCurrentTake ? mSidecarNoteEvents : mBasicPitch.getNoteEvents();
}

void TranscriptionManager::_runModel()
{
    mUsingSidecarForCurrentTake = false;

    const int requested_engine = jlimit(0,
                                        EngineCatalog::NumEngines - 1,
                                        static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::EngineId)));

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
        mBasicPitch.transcribeToMIDI(
            mProcessor->getSourceAudioManager()->getDownsampledSourceAudioForTranscription().getWritePointer(0),
            mProcessor->getSourceAudioManager()->getNumSamplesDownAcquired());
    }

    mRawNoteEventsRevision.fetch_add(1);

    mNoteOptions.setParameters(
        mProcessor->getParameterValue(ParameterHelpers::EnableNoteQuantizationId) > 0.5f,
        static_cast<NoteUtils::RootNote>(mProcessor->getParameterValue(ParameterHelpers::KeyRootNoteId)),
        static_cast<NoteUtils::ScaleType>(mProcessor->getParameterValue(ParameterHelpers::KeyTypeId)),
        static_cast<NoteUtils::SnapMode>(mProcessor->getParameterValue(ParameterHelpers::KeySnapModeId)),
        static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::MinMidiNoteId)),
        static_cast<int>(mProcessor->getParameterValue(ParameterHelpers::MaxMidiNoteId)));

    auto post_processed_notes = mNoteOptions.process(_rawNoteEvents());

    mTimeQuantizeOptions.setParameters(
        mProcessor->getParameterValue(ParameterHelpers::EnableTimeQuantizationId) > 0.5f,
        static_cast<TimeQuantizeUtils::TimeDivisions>(mProcessor->getParameterValue(ParameterHelpers::TimeDivisionId)),
        mProcessor->getParameterValue(ParameterHelpers::QuantizationForceId));

    mPostProcessedNotes = mTimeQuantizeOptions.quantize(post_processed_notes);

    Notes::dropOverlappingPitchBends(mPostProcessedNotes);
    Notes::mergeOverlappingNotesWithSamePitch(mPostProcessedNotes);

    // For the synth
    auto single_events = SynthController::buildMidiEventsVector(mPostProcessedNotes);
    mProcessor->getPlayer()->getSynthController()->setNewMidiEventsVectorToUse(single_events);

    mProcessor->setStateToPopulatedAudioAndMidiRegions();

    mTranscribeJobsInFlight.fetch_sub(1);
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

bool TranscriptionManager::isJobRunningOrQueued() const
{
    // Not mThreadPool.getNumJobs(): the sidecar probe shares that pool (deliberately, so it
    // cannot race a take for the client) and is not a transcription.
    return mTranscribeJobsInFlight.load() > 0;
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
        mTranscribeJobsInFlight.fetch_add(1);
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
