//
// Created by Tibor Vass on 04.03.23.
//

#include <algorithm>

#include "Notes.h"

bool Notes::Event::operator==(const Notes::Event& other) const
{
    return this->startTime == other.startTime && this->endTime == other.endTime && this->startFrame == other.startFrame
           && this->endFrame == other.endFrame && this->pitch == other.pitch && this->amplitude == other.amplitude
           && this->bends == other.bends;
}

namespace
{
/**
 * Zero a neighbouring pitch bin, conditionally.
 *
 * Spectral leakage into the bins either side of a note is real, and upstream suppresses it by
 * zeroing both of them across the note's whole duration. The cost is that a genuine minor second
 * cannot survive decoding at all: clusters, close voicings and any two-part writing that touches
 * a semitone lose a voice, structurally, every time. Two strong adjacent bins are two notes; one
 * strong and one weak is leakage, and only the second case wants zeroing.
 *
 * @param inRatio Fraction of the centre bin below which a neighbour counts as leakage. Negative
 *  restores the unconditional behaviour.
 */
inline void suppressNeighbour(float& ioNeighbour, float inCentre, float inRatio)
{
    if (inRatio < 0.0f || ioNeighbour < inRatio * inCentre) {
        ioNeighbour = 0.0f;
    }
}
} // namespace

std::vector<Notes::Event> Notes::convert(const std::vector<std::vector<float>>& inNotesPG,
                                         const std::vector<std::vector<float>>& inOnsetsPG,
                                         const std::vector<std::vector<float>>& inContoursPG,
                                         const ConvertParams& inParams,
                                         bool inNewAudio)
{
    std::vector<Event> events;
    events.reserve(1024);

    const auto n_frames = static_cast<int>(inNotesPG.size());
    if (n_frames == 0) {
        return events;
    }

    const auto n_notes = static_cast<int>(inNotesPG[0].size());
    assert(n_frames == inOnsetsPG.size());
    assert(n_frames == inContoursPG.size());
    assert(n_notes == inOnsetsPG[0].size());
    assert(n_notes == NUM_FREQ_OUT);

    std::vector<std::vector<float>> inferred_onsets;
    auto onsets_ptr = &inOnsetsPG;
    if (inParams.inferOnsets) {
        inferred_onsets = _inferredOnsets<float>(inOnsetsPG, inNotesPG);
        onsets_ptr = &inferred_onsets;
    }
    auto& onsets = *onsets_ptr;

    if (inNewAudio) {
        mRemainingEnergy = inNotesPG;
    } else {
        // Copy without changing the location of the original data
        assert(mRemainingEnergy.size() == n_frames);
        for (size_t f = 0; f < n_frames; f++) {
            assert(inNotesPG[f].size() == NUM_FREQ_OUT);
            assert(mRemainingEnergy[f].size() == NUM_FREQ_OUT);

            std::copy(inNotesPG[f].begin(), inNotesPG[f].end(), mRemainingEnergy[f].begin());
        }
    }

    if (inParams.melodiaTrick) {
        // Rebuild when the audio is new (mRemainingEnergy was reassigned above, so every pointer
        // held here dangles), and also when the index is simply absent, which happens if the
        // first convert() on this audio ran with the melodia trick off.
        if (inNewAudio || mRemainingEnergyIndex.empty()) {
            // Fill mRemainingEnergyIndex
            mRemainingEnergyIndex.clear();
            mRemainingEnergyIndex.reserve(static_cast<size_t>(n_frames) * static_cast<size_t>(NUM_FREQ_OUT));

            for (int frame_idx = 0; frame_idx < n_frames; frame_idx++) {
                for (int freq_idx = 0; freq_idx < NUM_FREQ_OUT; freq_idx++) {
                    mRemainingEnergyIndex.push_back(
                        {&mRemainingEnergy[static_cast<size_t>(frame_idx)][static_cast<size_t>(freq_idx)],
                         frame_idx,
                         freq_idx});
                }
            }

            mRemainingEnergyIndex.shrink_to_fit();

            // Sort here, once per take, rather than on every parameter change as upstream does.
            // The order is provably stable across tweaks: the only writes to mRemainingEnergy
            // during decoding assign zero, and every re-run restores it from inNotesPG, so the
            // descending order of the non-zero entries is always the descending order of the raw
            // posteriorgram, which does not change. The melodia walk below already skips zeros.
            // This matters because the index holds one record per (frame, pitch): five minutes of
            // audio is 2.27 million of them, about 36 MB, and re-sorting that on every knob turn
            // is the whole reason the sensitivity controls stall on long takes.
            std::sort(mRemainingEnergyIndex.begin(),
                      mRemainingEnergyIndex.end(),
                      [](const _pg_index& a, const _pg_index& b) { return *a.value > *b.value; });
        }
    }

    const auto frame_threshold = inParams.frameThreshold;
    // TODO: infer frame_threshold if < 0, can be merged with inferredOnsets.

    // constrain frequencies
    const auto max_note_idx =
        inParams.maxFrequency < 0 ? n_notes - 1 : NoteUtils::hzToMidi(inParams.maxFrequency) - MIDI_OFFSET;
    const auto min_note_idx = inParams.minFrequency < 0 ? 0 : NoteUtils::hzToMidi(inParams.minFrequency) - MIDI_OFFSET;

    // basic-pitch zeroes the posteriorgrams outside the requested range; bounding the loop below
    // is not the same thing. The melodia trick walks every band of mRemainingEnergy, so with the
    // range enforced only as a loop bound it happily emits notes the caller asked to exclude:
    // case 5 of the notes test (melodiaTrick on, 330-1567 Hz) returned 16 events against a golden
    // 9, the seven extras all sitting below the 330 Hz floor. Zeroing here is enough on its own,
    // because that walk skips any band whose energy is already zero. The onset posteriorgram
    // needs no matching pass: nothing reads it outside these bounds, and with inferOnsets off it
    // is the caller's array, not ours.
    const auto first_kept = std::max(0, min_note_idx);
    const auto last_kept = std::min(n_notes - 1, max_note_idx);
    for (size_t f = 0; f < static_cast<size_t>(n_frames); f++) {
        for (int note_idx = 0; note_idx < first_kept; note_idx++) {
            mRemainingEnergy[f][static_cast<size_t>(note_idx)] = 0.0f;
        }
        for (int note_idx = last_kept + 1; note_idx < n_notes; note_idx++) {
            mRemainingEnergy[f][static_cast<size_t>(note_idx)] = 0.0f;
        }
    }

    // stop 1 frame early to prevent edge case
    // as per https://github.com/spotify/basic-pitch/blob/f85a8e9ade1f297b8adb39b155c483e2312e1aca/basic_pitch/note_creation.py#L399
    const int last_frame = n_frames - 1;

    // Go backwards in time
    for (int frame_idx = last_frame - 1; frame_idx >= 0; frame_idx--) {
        for (int note_idx = max_note_idx; note_idx >= min_note_idx; note_idx--) {
            auto onset = onsets[frame_idx][note_idx];

            // equivalent to argrelmax logic
            auto prev = frame_idx <= 0 ? onset : onsets[frame_idx - 1][note_idx];
            auto next = frame_idx >= last_frame ? onset : onsets[frame_idx + 1][note_idx];

            if (onset < inParams.onsetThreshold || onset < prev || onset < next) {
                continue;
            }

            // Stage one: the note's core, found exactly as upstream finds it, against the one
            // global threshold. This is what decides whether the note exists, and it is left
            // alone on purpose. An earlier attempt folded the release below into this search and
            // the bench was unambiguous about it: letting a note ring down to a low floor lets a
            // short blip grow long enough to pass the minimum-length test, which cost 84 spurious
            // notes on a 60-note corpus and dropped precision from 0.59 to 0.30.
            int i = frame_idx + 1;
            int k = 0; // number of frames since energy dropped below threshold
            auto peak = mRemainingEnergy[frame_idx][note_idx];

            while (i < last_frame && k < inParams.energyThreshold) {
                const auto energy = mRemainingEnergy[i][note_idx];
                peak = std::max(peak, energy);

                if (energy < frame_threshold) {
                    k++;
                } else {
                    k = 0;
                }
                i++;
            }

            const auto core_end = i - k; // go back to frame above threshold

            // if the note is too short, skip it
            if (core_end - frame_idx <= inParams.minNoteLength) {
                continue;
            }

            // Stage two: follow the decay past the core, down to a fraction of the note's own
            // peak. Upstream has no decay model, no release and no pedal, so a piano note under
            // the sustain pedal is cut at an absolute floor long before the damper falls. Only
            // the reported offset moves here; existence was settled above. Clamped up to
            // frame_threshold so this can only extend a note, and floored at the take's measured
            // noise floor so a quiet note cannot chase noise to the end of the file.
            auto note_end = core_end;

            if (inParams.releaseRatio >= 0.0f) {
                const auto release =
                    std::min(frame_threshold, std::max(inParams.releaseFloor, inParams.releaseRatio * peak));

                int j = core_end;
                int below = 0;

                while (j < last_frame && below < inParams.energyThreshold) {
                    if (mRemainingEnergy[j][note_idx] < release) {
                        below++;
                    } else {
                        below = 0;
                    }
                    j++;
                }

                note_end = j - below;
            }

            double amplitude = 0.0;
            for (int f = frame_idx; f < note_end; f++) {
                const auto centre = mRemainingEnergy[f][note_idx];

                // Confidence is the mean over the core. The release tail is low energy by
                // definition, and averaging it in would report a note as less certain the longer
                // it was allowed to ring.
                if (f < core_end) {
                    amplitude += centre;
                }

                mRemainingEnergy[f][note_idx] = 0;

                if (note_idx < MAX_NOTE_IDX) {
                    suppressNeighbour(mRemainingEnergy[f][note_idx + 1], centre, inParams.neighbourSuppressionRatio);
                }
                if (note_idx > 0) {
                    suppressNeighbour(mRemainingEnergy[f][note_idx - 1], centre, inParams.neighbourSuppressionRatio);
                }
            }

            amplitude /= (core_end - frame_idx);

            // The onset peak sits between frames, and the grid is 11.6 ms, which is inside what a
            // listener hears as timing. prev and next are the neighbouring onset values the local
            // maximum test above already read, so the vertex costs nothing more to compute.
            const auto start_time = inParams.refineOnsetTiming
                                        ? _refinedFrameToTime(frame_idx + _subFrameOffset(prev, onset, next))
                                        : _modelFrameToTime(frame_idx);

            Event event {
                start_time /* startTime */,
                _modelFrameToTime(note_end) /* endTime */,
                frame_idx /* startFrame */,
                note_end /* endFrame */,
                note_idx + MIDI_OFFSET /* pitch */,
                amplitude /* amplitude */,
            };
            event.onsetConfidence = onset;

            events.push_back(std::move(event));
        }
    }

    if (inParams.melodiaTrick) {
        // this inhibit function zeroes out neighbor notes and keeps track (with k)
        // on how many consecutive frames were below frame_threshold.
        const auto suppression = inParams.neighbourSuppressionRatio;
        auto inhibit = [frame_threshold, suppression](
                           std::vector<std::vector<float>>& pg, int frame_i, int note_i, int k) {
            if (pg[frame_i][note_i] < frame_threshold) {
                k++;
            } else {
                k = 0;
            }

            const auto centre = pg[frame_i][note_i];
            pg[frame_i][note_i] = 0;
            if (note_i < MAX_NOTE_IDX) {
                suppressNeighbour(pg[frame_i][note_i + 1], centre, suppression);
            }
            if (note_i > 0) {
                suppressNeighbour(pg[frame_i][note_i - 1], centre, suppression);
            }
            return k;
        };

        // loop through each remaining note probability in descending order
        // until reaching frame_threshold. The order was established once, when the index was
        // built, and cannot have changed since: see the note there.
        for (auto& [energy_ptr, frame_idx, note_idx]: mRemainingEnergyIndex) {
            auto& energy = *energy_ptr;

            // skip those that have already been zeroed
            if (energy == 0.0f) {
                continue;
            }

            if (energy <= frame_threshold) {
                break;
            }
            energy = 0;

            // forward pass
            int i = frame_idx + 1;
            int k = 0;
            while (i < last_frame && k < inParams.energyThreshold) {
                k = inhibit(mRemainingEnergy, i, note_idx, k);
                i++;
            }

            const auto i_end = i - 1 - k;

            // backward pass
            i = frame_idx - 1;
            k = 0;
            while (i > 0 && k < inParams.energyThreshold) {
                k = inhibit(mRemainingEnergy, i, note_idx, k);
                i--;
            }

            const auto i_start = i + 1 + k;

            // if the note is too short, skip it
            if (i_end - i_start <= inParams.minNoteLength) {
                continue;
            }

            double amplitude = 0.0;
            for (i = i_start; i < i_end; i++) {
                amplitude += inNotesPG[i][note_idx];
            }
            amplitude /= (i_end - i_start);

            Event event {
                _modelFrameToTime(i_start /* startTime */),
                _modelFrameToTime(i_end) /* endTime */,
                i_start /* startFrame */,
                i_end /* endFrame */,
                note_idx + MIDI_OFFSET /* pitch */,
                amplitude /* amplitude */,
            };

            // No sub-frame refinement here, and deliberately so: these notes were recovered from
            // leftover energy rather than from an onset peak, so there is no local maximum to fit
            // a parabola through. Record what the onset posteriorgram had to say about the start
            // frame anyway, since a caller ranking notes by reliability wants to know.
            event.onsetConfidence = onsets[i_start][note_idx];

            events.push_back(std::move(event));
        }
    }

    sortEvents(events);

    if (inParams.pitchBend != NoPitchBend) {
        _addPitchBends(events, inContoursPG);
        if (inParams.pitchBend == SinglePitchBend) {
            dropOverlappingPitchBends(events);
        }
    }

    return events;
}

void Notes::clear()
{
    mRemainingEnergy.clear();
    mRemainingEnergy.shrink_to_fit();

    mRemainingEnergyIndex.clear();
    mRemainingEnergyIndex.shrink_to_fit();
}

void Notes::_addPitchBends(std::vector<Event>& inOutEvents,
                           const std::vector<std::vector<float>>& inContoursPG,
                           int inNumBinsTolerance)
{
    for (auto& event: inOutEvents) {
        // midi_pitch_to_contour_bin
        int note_idx =
            CONTOURS_BINS_PER_SEMITONE
            * (event.pitch - 69 + 12 * static_cast<int>(std::round(std::log2(440.0f / ANNOTATIONS_BASE_FREQUENCY))));

        static constexpr int N_FREQ_BINS_CONTOURS = NUM_FREQ_OUT * CONTOURS_BINS_PER_SEMITONE;
        int note_start_idx = std::max(note_idx - inNumBinsTolerance, 0);
        int note_end_idx = std::min(N_FREQ_BINS_CONTOURS, note_idx + inNumBinsTolerance + 1);

        const auto gauss_start = static_cast<float>(std::max(0, inNumBinsTolerance - note_idx));
        const auto pb_shift = inNumBinsTolerance - std::max(0, inNumBinsTolerance - note_idx);

        for (int i = event.startFrame; i < event.endFrame; i++) {
            int bend = 0;
            float max = 0;
            for (int j = note_start_idx; j < note_end_idx; j++) {
                int k = j - note_start_idx;
                float x = gauss_start + static_cast<float>(k);
                float n = x - static_cast<float>(inNumBinsTolerance);

                static constexpr float std = 5.0f;

                // Gaussian
                float w = std::exp(-(n * n) / (2.0f * std * std)) * inContoursPG[i][j];

                if (w > max) {
                    bend = k;
                    max = w;
                }
            }
            event.bends.emplace_back(bend - pb_shift);
        }
    }
}