//
// Quarry's transcription bench.
//
// "Not good enough" cannot be fixed without a number. This produces the number: note-level
// precision, recall and F1 against reference MIDI, over a corpus of paired files, in one command
// and one table, against a committed baseline.
//
// It deliberately runs the same engine the plugin runs, through the same preprocessing, rather
// than a convenient approximation of it. A bench that measures something other than the shipping
// path measures nothing.
//

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <JuceHeader.h>

#include "AudioUtils.h"
#include "BasicPitch.h"
#include "BasicPitchConstants.h"
#include "SidecarClient.h"

namespace
{
// The standard note-level onset tolerance in the transcription literature, and what mir_eval
// defaults to. Every number this tool prints is meaningless without it stated.
constexpr double kOnsetToleranceSeconds = 0.05;

// Offset tolerance is the larger of the onset tolerance and a fraction of the note's own length,
// because being 50 ms out on a semiquaver and on a whole note are not the same error.
constexpr double kOffsetToleranceRatio = 0.2;

// Velocity tolerance as a share of the reference take's loudest note, after the optimal global
// rescale below. Velocity is only ever recoverable up to a scale, so the scale is not the test.
constexpr double kVelocityTolerance = 0.1;

// The sustain-pedal onset tolerance Kong et al. use in their piano transcription evaluation
// ("High-Resolution Piano Transcription with Pedals by Regressing Onset and Offset Times", 2021):
// 200 ms, four times the note-onset tolerance above. A sustain gesture's edge is heard, not
// played to a click, so it gets a wider window than a struck note's onset does.
constexpr double kPedalOnsetToleranceSeconds = 0.2;

struct Note {
    double onset = 0.0;
    double offset = 0.0;
    int pitch = 0;
    double velocity = 0.0; // 0 to 127, to match MIDI on both sides
};

// One CC64 sample, in the sidecar's own units: seconds and a 0-127 value, exactly the range MIDI
// (and the ground-truth mid's CC64 events) already use.
struct PedalEvent {
    double time = 0.0;
    int value = 0;
};

// One pedal-down span, value >= 64 to value < 64, in excerpt-relative seconds.
struct PedalSpan {
    double onset = 0.0;
    double offset = 0.0;
};

struct Score {
    int matched = 0;
    int estimated = 0;
    int reference = 0;

    double precision() const { return estimated > 0 ? static_cast<double>(matched) / estimated : 0.0; }
    double recall() const { return reference > 0 ? static_cast<double>(matched) / reference : 0.0; }

    double f1() const
    {
        const auto p = precision();
        const auto r = recall();
        return (p + r) > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
    }
};

struct ItemResult {
    juce::String name;
    Score onsetOnly;
    Score onsetOffset;
    Score onsetVelocity;
    // Span-level pedal score (see scorePedalSpans). reference/estimated/matched all stay 0 when
    // the run has no pedal capability at all (no --sidecar); printRow tells that case apart from
    // a genuine zero-recall score by checking hasPedalCapability, not by looking at these counts.
    Score pedal;
    // Mean absolute onset error over the matched pairs, in milliseconds. F1 at a 50 ms tolerance
    // cannot see sub-frame onset refinement at all, because the frame grid is 11.6 ms and the
    // whole correction is smaller than a tenth of the tolerance. This can.
    double onsetErrorMs = 0.0;
    double seconds = 0.0;
};

/**
 * One augmenting-path step of Kuhn's algorithm for maximum bipartite matching.
 */
bool augment(int inLeft,
             const std::vector<std::vector<int>>& inAdjacency,
             std::vector<int>& ioMatchOfRight,
             std::vector<bool>& ioSeen)
{
    for (const auto right: inAdjacency[static_cast<size_t>(inLeft)]) {
        if (ioSeen[static_cast<size_t>(right)]) {
            continue;
        }

        ioSeen[static_cast<size_t>(right)] = true;

        if (ioMatchOfRight[static_cast<size_t>(right)] < 0
            || augment(ioMatchOfRight[static_cast<size_t>(right)], inAdjacency, ioMatchOfRight, ioSeen)) {
            ioMatchOfRight[static_cast<size_t>(right)] = inLeft;
            return true;
        }
    }

    return false;
}

/**
 * Maximum bipartite matching between reference and estimated notes.
 *
 * Greedy nearest-neighbour matching is the obvious shortcut here and it is wrong: it can consume
 * an estimate that was the only possible partner for a later reference note, and so under-reports
 * recall in exactly the dense passages the bench exists to measure. This is the same choice
 * mir_eval makes, for the same reason.
 *
 * @param outMatches Filled with (reference index, estimate index) pairs.
 * @return Number of matched pairs.
 */
int maximumMatching(const std::vector<std::vector<int>>& inAdjacency,
                    size_t inNumEstimates,
                    std::vector<std::pair<int, int>>& outMatches)
{
    std::vector<int> match_of_right(inNumEstimates, -1);
    int matched = 0;

    for (size_t left = 0; left < inAdjacency.size(); left++) {
        std::vector<bool> seen(inNumEstimates, false);

        if (augment(static_cast<int>(left), inAdjacency, match_of_right, seen)) {
            matched++;
        }
    }

    outMatches.clear();

    for (size_t right = 0; right < match_of_right.size(); right++) {
        if (match_of_right[right] >= 0) {
            outMatches.emplace_back(match_of_right[right], static_cast<int>(right));
        }
    }

    return matched;
}

/**
 * Candidate pairs under the onset criterion: same pitch, onsets within tolerance.
 */
std::vector<std::vector<int>> onsetAdjacency(const std::vector<Note>& inReference, const std::vector<Note>& inEstimate)
{
    std::vector<std::vector<int>> adjacency(inReference.size());

    for (size_t r = 0; r < inReference.size(); r++) {
        for (size_t e = 0; e < inEstimate.size(); e++) {
            if (inReference[r].pitch != inEstimate[e].pitch) {
                continue;
            }

            if (std::abs(inReference[r].onset - inEstimate[e].onset) <= kOnsetToleranceSeconds) {
                adjacency[r].push_back(static_cast<int>(e));
            }
        }
    }

    return adjacency;
}

/**
 * Score under the onset criterion alone.
 */
Score scoreOnsets(const std::vector<Note>& inReference,
                  const std::vector<Note>& inEstimate,
                  std::vector<std::pair<int, int>>& outMatches)
{
    Score score;
    score.reference = static_cast<int>(inReference.size());
    score.estimated = static_cast<int>(inEstimate.size());
    score.matched = maximumMatching(onsetAdjacency(inReference, inEstimate), inEstimate.size(), outMatches);

    return score;
}

/**
 * Score under the onset-and-offset criterion.
 */
Score scoreOnsetsAndOffsets(const std::vector<Note>& inReference, const std::vector<Note>& inEstimate)
{
    auto adjacency = onsetAdjacency(inReference, inEstimate);

    for (size_t r = 0; r < inReference.size(); r++) {
        const auto duration = inReference[r].offset - inReference[r].onset;
        const auto tolerance = std::max(kOnsetToleranceSeconds, kOffsetToleranceRatio * duration);

        auto& candidates = adjacency[r];
        candidates.erase(std::remove_if(candidates.begin(),
                                        candidates.end(),
                                        [&](int e) {
                                            const auto& estimate = inEstimate[static_cast<size_t>(e)];
                                            return std::abs(inReference[r].offset - estimate.offset) > tolerance;
                                        }),
                         candidates.end());
    }

    Score score;
    score.reference = static_cast<int>(inReference.size());
    score.estimated = static_cast<int>(inEstimate.size());

    std::vector<std::pair<int, int>> matches;
    score.matched = maximumMatching(adjacency, inEstimate.size(), matches);

    return score;
}

/**
 * Score under the onset-and-velocity criterion.
 *
 * Velocities are compared only after rescaling the estimates by the single factor that best fits
 * the reference, because no transcriber can recover absolute loudness and none is being asked to.
 * What is being asked is whether the relative dynamics are right: whether the accent landed on the
 * note that was accented. The scale is fitted on the onset-matched pairs, as mir_eval does.
 */
Score scoreOnsetsAndVelocity(const std::vector<Note>& inReference, const std::vector<Note>& inEstimate)
{
    std::vector<std::pair<int, int>> onset_matches;
    scoreOnsets(inReference, inEstimate, onset_matches);

    double numerator = 0.0;
    double denominator = 0.0;

    for (const auto& match: onset_matches) {
        const auto& reference = inReference[static_cast<size_t>(match.first)];
        const auto& estimate = inEstimate[static_cast<size_t>(match.second)];

        numerator += reference.velocity * estimate.velocity;
        denominator += estimate.velocity * estimate.velocity;
    }

    const auto scale = denominator > 0.0 ? numerator / denominator : 0.0;

    double loudest = 0.0;

    for (const auto& note: inReference) {
        loudest = std::max(loudest, note.velocity);
    }

    const auto tolerance = kVelocityTolerance * loudest;

    auto adjacency = onsetAdjacency(inReference, inEstimate);

    for (size_t r = 0; r < inReference.size(); r++) {
        auto& candidates = adjacency[r];
        candidates.erase(std::remove_if(candidates.begin(),
                                        candidates.end(),
                                        [&](int e) {
                                            const auto scaled = scale * inEstimate[static_cast<size_t>(e)].velocity;
                                            return std::abs(inReference[r].velocity - scaled) > tolerance;
                                        }),
                         candidates.end());
    }

    Score score;
    score.reference = static_cast<int>(inReference.size());
    score.estimated = static_cast<int>(inEstimate.size());

    std::vector<std::pair<int, int>> matches;
    score.matched = maximumMatching(adjacency, inEstimate.size(), matches);

    return score;
}

/**
 * Candidate pairs under the pedal-onset criterion: onsets within kPedalOnsetToleranceSeconds.
 * No pitch to match on (there is one pedal channel, not 128), so this is onsetAdjacency with
 * that one constraint dropped.
 */
std::vector<std::vector<int>> pedalOnsetAdjacency(const std::vector<PedalSpan>& inReference,
                                                   const std::vector<PedalSpan>& inEstimate)
{
    std::vector<std::vector<int>> adjacency(inReference.size());

    for (size_t r = 0; r < inReference.size(); r++) {
        for (size_t e = 0; e < inEstimate.size(); e++) {
            if (std::abs(inReference[r].onset - inEstimate[e].onset) <= kPedalOnsetToleranceSeconds) {
                adjacency[r].push_back(static_cast<int>(e));
            }
        }
    }

    return adjacency;
}

/**
 * Score pedal-down spans the same way scoreOnsetsAndOffsets scores notes: bipartite-matched on
 * onset within tolerance, then filtered to also require the offset within max(that tolerance,
 * kOffsetToleranceRatio of the reference span's own length) -- the same 20% rule notes use, just
 * measured against the wider pedal onset tolerance rather than the note one.
 */
Score scorePedalSpans(const std::vector<PedalSpan>& inReference, const std::vector<PedalSpan>& inEstimate)
{
    auto adjacency = pedalOnsetAdjacency(inReference, inEstimate);

    for (size_t r = 0; r < inReference.size(); r++) {
        const auto duration = inReference[r].offset - inReference[r].onset;
        const auto tolerance = std::max(kPedalOnsetToleranceSeconds, kOffsetToleranceRatio * duration);

        auto& candidates = adjacency[r];
        candidates.erase(std::remove_if(candidates.begin(),
                                        candidates.end(),
                                        [&](int e) {
                                            const auto& estimate = inEstimate[static_cast<size_t>(e)];
                                            return std::abs(inReference[r].offset - estimate.offset) > tolerance;
                                        }),
                         candidates.end());
    }

    Score score;
    score.reference = static_cast<int>(inReference.size());
    score.estimated = static_cast<int>(inEstimate.size());

    std::vector<std::pair<int, int>> matches;
    score.matched = maximumMatching(adjacency, inEstimate.size(), matches);

    return score;
}

/**
 * Segment a CC64 stream into pedal-down spans: a value >= 64 opens a span, a value < 64 closes
 * it. A span still open at inDuration is closed there rather than left dangling, so a pedal held
 * through the end of the excerpt still scores as one bounded span ("clip to the excerpt"). Events
 * are trusted to already be in time order, as both the ground-truth mid and the sidecar protocol
 * guarantee.
 */
std::vector<PedalSpan> pedalSpans(const std::vector<PedalEvent>& inEvents, double inDuration)
{
    std::vector<PedalSpan> spans;
    bool down = false;
    double open_time = 0.0;

    for (const auto& event: inEvents) {
        const auto t = juce::jlimit(0.0, inDuration, event.time);
        const auto is_down = event.value >= 64;

        if (is_down && !down) {
            down = true;
            open_time = t;
        } else if (!is_down && down) {
            down = false;

            if (t > open_time) {
                spans.push_back({open_time, t});
            }
        }
    }

    if (down && inDuration > open_time) {
        spans.push_back({open_time, inDuration});
    }

    return spans;
}

/**
 * Read note and CC64 (sustain pedal) events out of a reference MIDI file, in seconds.
 */
bool loadReference(const juce::File& inFile, std::vector<Note>& outNotes, std::vector<PedalEvent>& outPedal)
{
    juce::FileInputStream stream(inFile);

    if (!stream.openedOk()) {
        return false;
    }

    juce::MidiFile midi;

    if (!midi.readFrom(stream)) {
        return false;
    }

    // Without this the timestamps are ticks and any tempo map in the file is ignored.
    midi.convertTimestampTicksToSeconds();

    outNotes.clear();
    outPedal.clear();

    for (int track_idx = 0; track_idx < midi.getNumTracks(); track_idx++) {
        auto track = *midi.getTrack(track_idx);
        track.updateMatchedPairs();

        for (int i = 0; i < track.getNumEvents(); i++) {
            const auto& message = track.getEventPointer(i)->message;

            if (message.isNoteOn()) {
                const auto note_off = track.getTimeOfMatchingKeyUp(i);

                Note note;
                note.onset = message.getTimeStamp();
                // A note-on with no matching key-up is a malformed file, not a note of zero length.
                note.offset = note_off > note.onset ? note_off : message.getTimeStamp();
                note.pitch = message.getNoteNumber();
                note.velocity = message.getVelocity();

                outNotes.push_back(note);
            } else if (message.isController() && message.getControllerNumber() == 64) {
                PedalEvent event;
                event.time = message.getTimeStamp();
                event.value = message.getControllerValue();

                outPedal.push_back(event);
            }
        }
    }

    std::sort(outNotes.begin(), outNotes.end(), [](const Note& a, const Note& b) {
        return a.onset < b.onset || (a.onset == b.onset && a.pitch < b.pitch);
    });

    std::stable_sort(outPedal.begin(), outPedal.end(), [](const PedalEvent& a, const PedalEvent& b) {
        return a.time < b.time;
    });

    return true;
}

/**
 * Build the bench's own Note rows from Notes::Event, the currency both BasicPitch and the
 * sidecar client (via toNotesEvents) produce. Shared so the sidecar path scores exactly the same
 * way the BasicPitch path always has.
 */
std::vector<Note> notesFromEvents(const std::vector<Notes::Event>& inEvents)
{
    std::vector<Note> notes;
    notes.reserve(inEvents.size());

    for (const auto& event: inEvents) {
        Note note;
        note.onset = event.startTime;
        note.offset = event.endTime;
        note.pitch = event.pitch;
        note.velocity = event.velocity * 127.0;

        notes.push_back(note);
    }

    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return a.onset < b.onset || (a.onset == b.onset && a.pitch < b.pitch);
    });

    return notes;
}

/**
 * Map the sidecar's own pedal event shape onto the bench's PedalEvent, which is that same shape:
 * kept as a named conversion (rather than reusing SidecarPedalEvent directly downstream) so the
 * pedal-scoring code below reads the same regardless of where the events came from, the same
 * reason notesFromEvents exists for notes.
 */
std::vector<PedalEvent> pedalFromSidecarEvents(const std::vector<SidecarPedalEvent>& inEvents)
{
    std::vector<PedalEvent> events;
    events.reserve(inEvents.size());

    for (const auto& event: inEvents) {
        events.push_back({event.time, event.value});
    }

    return events;
}

/**
 * The excerpt's own duration in seconds, read from the audio file's header alone (no decode),
 * so pedalSpans has a boundary to clip a still-down span to. Falls back to a full decode only for
 * formats createReaderFor doesn't cover (mp3; see AudioUtils::loadAudioFile), which no pedal
 * corpus currently uses.
 */
double excerptDurationSeconds(const juce::File& inFile)
{
    auto format_manager = AudioUtils::createAudioFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(format_manager->createReaderFor(inFile));

    if (reader && reader->sampleRate > 0.0) {
        return static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    }

    juce::AudioBuffer<float> buffer;
    double sample_rate = 0.0;

    if (AudioUtils::loadAudioFile(inFile, buffer, sample_rate) && sample_rate > 0.0) {
        return static_cast<double>(buffer.getNumSamples()) / sample_rate;
    }

    return 0.0;
}

/**
 * Transcribe one file through the same preprocessing and the same engine the plugin uses.
 */
bool transcribe(const juce::File& inFile, bool inLegacy, std::vector<Note>& outNotes, double& outSeconds)
{
    juce::AudioBuffer<float> source;
    double sample_rate = 0.0;

    if (!AudioUtils::loadAudioFile(inFile, source, sample_rate) || source.getNumSamples() == 0) {
        return false;
    }

    AudioUtils::downmixToMono(source);

    juce::AudioBuffer<float> downsampled;
    AudioUtils::resampleBuffer(source, downsampled, sample_rate, BASIC_PITCH_SAMPLE_RATE);

    BasicPitch engine;

    if (inLegacy) {
        // The shipped defaults as they were: 0.7 and 0.5 mapped straight onto thresholds of 0.3
        // and 0.5, with a 125 ms minimum note duration.
        engine.setLegacyEngine(true);
        engine.setParameters(0.7f, 0.5f, 125.0f);
    } else {
        // The knobs at their neutral defaults, which is the entire point: this measures what a
        // user gets on drop, not what a user gets after tuning by hand for this one file.
        engine.setParameters(0.5f, 0.5f, 75.0f);
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();
    engine.transcribeToMIDI(downsampled.getWritePointer(0), downsampled.getNumSamples());
    outSeconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;

    outNotes = notesFromEvents(engine.getNoteEvents());

    return true;
}

/**
 * Transcribe one file through a sidecar process instead of BasicPitch, for --sidecar. Goes
 * through the same SidecarNote -> Notes::Event -> Note pipeline the plugin will eventually use,
 * so this exercises exactly the conversion the real integration relies on rather than a shortcut.
 * Also hands back the sidecar's raw pedal (CC64) stream in outPedal: BasicPitch has no pedal
 * output at all, so this is the only path that ever produces one.
 */
bool transcribeWithSidecar(SidecarClient& ioClient,
                          const juce::String& inEngine,
                          const juce::File& inFile,
                          std::vector<Note>& outNotes,
                          std::vector<SidecarPedalEvent>& outPedal,
                          double& outSeconds)
{
    std::vector<SidecarNote> sidecar_notes;
    juce::String error;

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto ok = ioClient.transcribe(inFile, inEngine, sidecar_notes, outPedal, error);
    outSeconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;

    if (!ok) {
        std::cerr << "  sidecar error on " << inFile.getFileName().toStdString() << ": " << error.toStdString()
                  << std::endl;
        return false;
    }

    outNotes = notesFromEvents(toNotesEvents(sidecar_notes));

    return true;
}

juce::String cell(double inValue)
{
    return juce::String(inValue, 3).paddedLeft(' ', 7);
}

/**
 * The pedal column: "-" when this run has no pedal capability at all (no --sidecar, so nothing
 * was ever collected to score) or when the reference excerpt simply has no pedal spans (nothing
 * to measure recall against); a real F1 -- 0.000 included -- whenever the reference has spans and
 * the run could in principle have matched them, whether or not the engine actually produced any
 * for this case. That is what keeps a genuine zero-recall score (sidecar ran, engine emitted no
 * pedal, reference had some) visibly different from "not measured" (no sidecar at all).
 */
juce::String pedalCell(bool inHasPedalCapability, const Score& inPedal)
{
    if (!inHasPedalCapability || inPedal.reference == 0) {
        return juce::String("-").paddedLeft(' ', 7);
    }

    return cell(inPedal.f1());
}

void printRow(const juce::String& inName, const ItemResult& inResult, bool inHasPedalCapability)
{
    std::cout << inName.paddedRight(' ', 20).toStdString() << cell(inResult.onsetOnly.precision()).toStdString()
              << cell(inResult.onsetOnly.recall()).toStdString() << cell(inResult.onsetOnly.f1()).toStdString()
              << cell(inResult.onsetOffset.f1()).toStdString() << cell(inResult.onsetVelocity.f1()).toStdString()
              << juce::String(inResult.onsetErrorMs, 1).paddedLeft(' ', 8).toStdString()
              << juce::String(inResult.onsetOnly.reference).paddedLeft(' ', 7).toStdString()
              << juce::String(inResult.onsetOnly.estimated).paddedLeft(' ', 7).toStdString()
              << pedalCell(inHasPedalCapability, inResult.pedal).toStdString() << std::endl;
}

/**
 * Aggregate across the corpus by summing counts rather than averaging per-file F1, so a long
 * difficult take is not outvoted by a short easy one.
 */
ItemResult aggregate(const std::vector<ItemResult>& inResults)
{
    ItemResult total;
    total.name = "ALL";

    for (const auto& result: inResults) {
        total.onsetOnly.matched += result.onsetOnly.matched;
        total.onsetOnly.estimated += result.onsetOnly.estimated;
        total.onsetOnly.reference += result.onsetOnly.reference;

        total.onsetOffset.matched += result.onsetOffset.matched;
        total.onsetOffset.estimated += result.onsetOffset.estimated;
        total.onsetOffset.reference += result.onsetOffset.reference;

        total.onsetVelocity.matched += result.onsetVelocity.matched;
        total.onsetVelocity.estimated += result.onsetVelocity.estimated;
        total.onsetVelocity.reference += result.onsetVelocity.reference;

        total.pedal.matched += result.pedal.matched;
        total.pedal.estimated += result.pedal.estimated;
        total.pedal.reference += result.pedal.reference;

        total.onsetErrorMs += result.onsetErrorMs * result.onsetOnly.matched;
        total.seconds += result.seconds;
    }

    if (total.onsetOnly.matched > 0) {
        total.onsetErrorMs /= total.onsetOnly.matched;
    }

    return total;
}

/**
 * Baselines are tab separated on purpose: one line per item, so a regression shows up in a diff
 * as the line that moved rather than as a reformatted blob.
 */
juce::String toBaseline(const std::vector<ItemResult>& inResults, const ItemResult& inTotal)
{
    juce::String out;
    out << "# name\tonset_f1\tonset_offset_f1\tonset_velocity_f1\tonset_err_ms\tref_notes\test_notes\n";

    auto line = [&out](const ItemResult& r) {
        out << r.name << "\t" << juce::String(r.onsetOnly.f1(), 4) << "\t" << juce::String(r.onsetOffset.f1(), 4)
            << "\t" << juce::String(r.onsetVelocity.f1(), 4) << "\t" << juce::String(r.onsetErrorMs, 2) << "\t"
            << r.onsetOnly.reference << "\t" << r.onsetOnly.estimated << "\n";
    };

    for (const auto& result: inResults) {
        line(result);
    }

    line(inTotal);

    return out;
}

/**
 * Write one case's transcribed notes to a TSV file, so they can be scored by something other
 * than this bench (mir_eval, another transcriber's own scorer) without re-running the engine.
 * Velocity is on the 0-127 MIDI scale: the same value scoreOnsetsAndVelocity above matches
 * against the reference, before its own optimal rescale.
 */
bool writeNotesTsv(const juce::File& inFile, const std::vector<Note>& inNotes)
{
    juce::String out;
    out << "onset_s\toffset_s\tpitch\tvelocity\n";

    for (const auto& note: inNotes) {
        out << juce::String(note.onset, 6) << "\t" << juce::String(note.offset, 6) << "\t" << note.pitch << "\t"
            << juce::String(note.velocity, 3) << "\n";
    }

    return inFile.replaceWithText(out);
}

/**
 * Write one case's raw sidecar pedal stream to a TSV file, for --dump-notes: the engine's own
 * CC64 events, unsegmented, so a span-boundary disagreement can be checked against the actual
 * samples rather than just the derived F1. Only ever called when the sidecar produced pedal.
 */
bool writePedalTsv(const juce::File& inFile, const std::vector<SidecarPedalEvent>& inPedal)
{
    juce::String out;
    out << "time\tvalue\n";

    for (const auto& event: inPedal) {
        out << juce::String(event.time, 6) << "\t" << event.value << "\n";
    }

    return inFile.replaceWithText(out);
}

std::map<juce::String, double> readBaselineF1(const juce::File& inFile)
{
    std::map<juce::String, double> baseline;

    if (!inFile.existsAsFile()) {
        return baseline;
    }

    for (const auto& raw: juce::StringArray::fromLines(inFile.loadFileAsString())) {
        const auto line = raw.trim();

        if (line.isEmpty() || line.startsWith("#")) {
            continue;
        }

        const auto fields = juce::StringArray::fromTokens(line, "\t", "");

        if (fields.size() >= 2) {
            baseline[fields[0]] = fields[1].getDoubleValue();
        }
    }

    return baseline;
}

/**
 * Take the value of an option, accepting both --option=value and --option value.
 *
 * Done by hand because juce::ArgumentList::removeValueForOption understands only the first form
 * and fails badly on the second: it consumes the option and returns an empty string, so a
 * fallback that runs afterwards finds nothing left to look at. The visible symptom was the bench
 * running normally and quietly writing no baseline at all.
 */
juce::String takeOptionValue(juce::ArgumentList& ioArgs, juce::StringRef inOption)
{
    const auto index = ioArgs.indexOfOption(inOption);

    if (index < 0) {
        return {};
    }

    const auto text = ioArgs.arguments[index].text;
    ioArgs.arguments.remove(index);

    if (text.contains("=")) {
        return text.fromFirstOccurrenceOf("=", false, false);
    }

    if (index < ioArgs.arguments.size()) {
        const auto value = ioArgs.arguments[index].text;
        ioArgs.arguments.remove(index);
        return value;
    }

    return {};
}

void printUsage()
{
    std::cout << "Quarry transcription bench\n\n"
              << "  Bench <corpus-dir> [--baseline <file>] [--write-baseline <file>] [--dump-notes <dir>]\n"
              << "                     [--sidecar \"<command line>\"] [--engine <name>]\n\n"
              << "  <corpus-dir> holds pairs of <name>.wav (or .aiff/.flac/.ogg/.mp3) and <name>.mid.\n"
              << "  Reports note-level precision, recall and F1 at a 50 ms onset tolerance, three\n"
              << "  ways: onset only, onset and offset, onset and velocity. Also reports span-level\n"
              << "  sustain-pedal F1 against the reference mid's own CC64 stream, printed as \"-\"\n"
              << "  when the run has no pedal capability (no --sidecar) or the reference excerpt has\n"
              << "  no pedal spans; only --sidecar ever produces pedal to score against it.\n\n"
              << "  With --baseline it prints the change against a committed run and exits non-zero\n"
              << "  if aggregate onset F1 has fallen.\n\n"
              << "  With --dump-notes it writes each case's transcribed notes to <dir>/<name>.est.tsv,\n"
              << "  and, when the run produced pedal, its raw CC64 stream to <dir>/<name>.pedal.tsv,\n"
              << "  creating <dir> if needed.\n\n"
              << "  With --sidecar every case is transcribed through that command instead of\n"
              << "  BasicPitch: the process is launched once, sent one transcribe request per case over\n"
              << "  the sidecar protocol, and shut down at the end. --engine selects the sidecar's\n"
              << "  engine (default \"auto\"); ignored without --sidecar.\n";
}
} // namespace

int main(int argc, char* argv[])
{
    juce::ArgumentList args(argc, argv);

    if (args.size() < 1 || args.containsOption("--help|-h")) {
        printUsage();
        return args.size() < 1 ? 1 : 0;
    }

    const auto legacy = args.removeOptionIfFound("--legacy");
    const auto baseline_path = takeOptionValue(args, "--baseline");
    const auto write_baseline_path = takeOptionValue(args, "--write-baseline");
    const auto dump_notes_path = takeOptionValue(args, "--dump-notes");
    const auto sidecar_command = takeOptionValue(args, "--sidecar");
    const auto sidecar_engine_option = takeOptionValue(args, "--engine");
    const auto sidecar_engine = sidecar_engine_option.isNotEmpty() ? sidecar_engine_option : juce::String("auto");

    if (args.size() < 1) {
        printUsage();
        return 1;
    }

    const auto corpus = juce::File::getCurrentWorkingDirectory().getChildFile(args[0].text);

    if (!corpus.isDirectory()) {
        std::cerr << "Not a directory: " << corpus.getFullPathName().toStdString() << std::endl;
        return 1;
    }

    juce::File dump_notes_dir;

    if (dump_notes_path.isNotEmpty()) {
        dump_notes_dir = juce::File::getCurrentWorkingDirectory().getChildFile(dump_notes_path);
        dump_notes_dir.createDirectory();
    }

    std::unique_ptr<SidecarClient> sidecar_client;

    if (sidecar_command.isNotEmpty()) {
        sidecar_client = std::make_unique<SidecarClient>(sidecar_command);
        juce::String sidecar_error;

        if (!sidecar_client->start(sidecar_error)) {
            std::cerr << "Could not start sidecar: " << sidecar_error.toStdString() << std::endl;
            return 1;
        }
    }

    juce::Array<juce::File> audio_files;

    for (const auto& extension: AudioUtils::getSupportedAudioFileExtensions()) {
        corpus.findChildFiles(audio_files, juce::File::findFiles, false, "*" + extension);
    }

    audio_files.sort();

    std::vector<ItemResult> results;

    for (const auto& audio_file: audio_files) {
        const auto reference_file = audio_file.getSiblingFile(audio_file.getFileNameWithoutExtension() + ".mid");

        if (!reference_file.existsAsFile()) {
            std::cerr << "  skipping " << audio_file.getFileName().toStdString() << ": no matching .mid" << std::endl;
            continue;
        }

        std::vector<Note> reference;
        std::vector<PedalEvent> reference_pedal;

        if (!loadReference(reference_file, reference, reference_pedal)) {
            std::cerr << "  skipping " << audio_file.getFileName().toStdString() << ": unreadable reference"
                      << std::endl;
            continue;
        }

        std::vector<Note> estimate;
        std::vector<SidecarPedalEvent> sidecar_pedal; // Only the sidecar path ever fills this in.
        double seconds = 0.0;

        const auto transcribed = sidecar_client
            ? transcribeWithSidecar(*sidecar_client, sidecar_engine, audio_file, estimate, sidecar_pedal, seconds)
            : transcribe(audio_file, legacy, estimate, seconds);

        if (!transcribed) {
            std::cerr << "  skipping " << audio_file.getFileName().toStdString() << ": could not transcribe"
                      << std::endl;
            continue;
        }

        ItemResult result;
        result.name = audio_file.getFileNameWithoutExtension();
        result.seconds = seconds;

        if (dump_notes_path.isNotEmpty()) {
            const auto out_file = dump_notes_dir.getChildFile(result.name + ".est.tsv");

            if (!writeNotesTsv(out_file, estimate)) {
                std::cerr << "  could not write " << out_file.getFullPathName().toStdString() << std::endl;
            }

            if (!sidecar_pedal.empty()) {
                const auto pedal_file = dump_notes_dir.getChildFile(result.name + ".pedal.tsv");

                if (!writePedalTsv(pedal_file, sidecar_pedal)) {
                    std::cerr << "  could not write " << pedal_file.getFullPathName().toStdString() << std::endl;
                }
            }
        }

        std::vector<std::pair<int, int>> matches;
        result.onsetOnly = scoreOnsets(reference, estimate, matches);

        double error = 0.0;

        for (const auto& match: matches) {
            error += std::abs(reference[static_cast<size_t>(match.first)].onset
                              - estimate[static_cast<size_t>(match.second)].onset);
        }

        result.onsetErrorMs = matches.empty() ? 0.0 : 1000.0 * error / static_cast<double>(matches.size());

        result.onsetOffset = scoreOnsetsAndOffsets(reference, estimate);
        result.onsetVelocity = scoreOnsetsAndVelocity(reference, estimate);

        const auto excerpt_duration = excerptDurationSeconds(audio_file);
        const auto reference_pedal_spans = pedalSpans(reference_pedal, excerpt_duration);
        const auto estimated_pedal_spans = pedalSpans(pedalFromSidecarEvents(sidecar_pedal), excerpt_duration);
        result.pedal = scorePedalSpans(reference_pedal_spans, estimated_pedal_spans);

        results.push_back(result);
    }

    if (sidecar_client) {
        sidecar_client->shutdown();
    }

    // Whether this run could have produced pedal at all: only --sidecar ever fills sidecar_pedal
    // in, so a plain BasicPitch run has no pedal signal to score against the reference regardless
    // of what that reference contains. printRow uses this to tell "not measured" apart from a
    // genuine zero score.
    const auto has_pedal_capability = sidecar_client != nullptr;

    if (results.empty()) {
        std::cerr << "No usable pairs in " << corpus.getFullPathName().toStdString() << std::endl;
        return 1;
    }

    const auto total = aggregate(results);

    const auto engine_label = sidecar_client ? "ENGINE: sidecar (" + sidecar_engine + ")"
                                             : juce::String(legacy ? "ENGINE: legacy (pre-fix)" : "ENGINE: current");

    std::cout << std::endl
              << juce::String("item").paddedRight(' ', 20).toStdString()
              << juce::String("P").paddedLeft(' ', 7).toStdString()
              << juce::String("R").paddedLeft(' ', 7).toStdString()
              << juce::String("F1").paddedLeft(' ', 7).toStdString()
              << juce::String("+off").paddedLeft(' ', 7).toStdString()
              << juce::String("+vel").paddedLeft(' ', 7).toStdString()
              << juce::String("err ms").paddedLeft(' ', 8).toStdString()
              << juce::String("ref").paddedLeft(' ', 7).toStdString()
              << juce::String("est").paddedLeft(' ', 7).toStdString()
              << juce::String("pedal").paddedLeft(' ', 7).toStdString() << std::endl
              << std::string(84, '-') << std::endl;

    for (const auto& result: results) {
        printRow(result.name, result, has_pedal_capability);
    }

    std::cout << std::string(84, '-') << std::endl;
    printRow("ALL", total, has_pedal_capability);

    std::cout << std::endl
              << "onset tolerance 50 ms; offset tolerance max(that, " << kOffsetToleranceRatio
              << " x note length); velocity" << std::endl
              << "tolerance " << kVelocityTolerance << " of the take's peak, after the optimal global rescale."
              << std::endl
              << "pedal span onset tolerance " << (kPedalOnsetToleranceSeconds * 1000.0)
              << " ms (Kong et al.); pedal offset tolerance max(that, " << kOffsetToleranceRatio
              << " x span length)." << std::endl
              << engine_label.toStdString() << std::endl
              << "transcribed " << results.size() << " items in " << juce::String(total.seconds, 2).toStdString()
              << " s" << std::endl;

    int exit_code = 0;

    if (baseline_path.isNotEmpty()) {
        const auto baseline = readBaselineF1(juce::File::getCurrentWorkingDirectory().getChildFile(baseline_path));

        if (baseline.empty()) {
            std::cerr << std::endl << "No baseline found at " << baseline_path.toStdString() << std::endl;
            exit_code = 1;
        } else {
            std::cout << std::endl << "change in onset F1 against baseline:" << std::endl;

            for (const auto& result: results) {
                const auto it = baseline.find(result.name);
                std::cout << "  " << result.name.paddedRight(' ', 20).toStdString()
                          << (it == baseline.end()
                                  ? juce::String("new").paddedLeft(' ', 8)
                                  : juce::String(result.onsetOnly.f1() - it->second, 4).paddedLeft(' ', 8))
                                 .toStdString()
                          << std::endl;
            }

            const auto it = baseline.find("ALL");

            if (it != baseline.end()) {
                const auto delta = total.onsetOnly.f1() - it->second;

                std::cout << "  " << juce::String("ALL").paddedRight(' ', 20).toStdString()
                          << juce::String(delta, 4).paddedLeft(' ', 8).toStdString() << std::endl;

                // A bench that cannot fail is a bench nobody reads.
                if (delta < -0.001) {
                    std::cerr << std::endl << "REGRESSION: aggregate onset F1 fell by " << -delta << std::endl;
                    exit_code = 2;
                }
            }
        }
    }

    if (write_baseline_path.isNotEmpty()) {
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(write_baseline_path);

        if (out.replaceWithText(toBaseline(results, total))) {
            std::cout << std::endl << "wrote baseline to " << out.getFullPathName().toStdString() << std::endl;
        } else {
            std::cerr << std::endl << "could not write " << out.getFullPathName().toStdString() << std::endl;
            exit_code = 1;
        }
    }

    return exit_code;
}
