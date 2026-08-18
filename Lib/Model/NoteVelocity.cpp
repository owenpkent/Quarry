//
// Loudness for note events, measured from the audio rather than read off the model.
//

#include <algorithm>

#include "NoteVelocity.h"

void NoteVelocity::prepare(const float* inStackedCQT, size_t inNumFrames)
{
    clear();

    if (inStackedCQT == nullptr || inNumFrames == 0) {
        return;
    }

    mPitchEnergy.assign(inNumFrames, std::vector<float>(static_cast<size_t>(NUM_FREQ_OUT), 0.0f));

    for (size_t frame = 0; frame < inNumFrames; frame++) {
        const float* frame_data = inStackedCQT + frame * NUM_FREQ_IN * NUM_HARMONICS;

        for (int note_idx = 0; note_idx < NUM_FREQ_OUT; note_idx++) {
            // The CQT runs CONTOURS_BINS_PER_SEMITONE bins to the semitone up from A0, and A0 is
            // MIDI_OFFSET, so the fundamental of a pitch lands on the first bin of its semitone.
            // This is the same mapping Notes::_addPitchBends derives the long way round.
            const int fundamental_bin = note_idx * CONTOURS_BINS_PER_SEMITONE;
            const int first_bin = std::max(0, fundamental_bin - kBinTolerance);
            const int last_bin = std::min(NUM_FREQ_IN - 1, fundamental_bin + kBinTolerance);

            float energy = 0.0f;

            for (int harmonic = kFirstHarmonic; harmonic <= kLastHarmonic; harmonic++) {
                // Take the strongest bin in the tolerance window rather than their sum, so a
                // slightly sharp string reads as one loud partial and not as several quiet ones.
                float strongest = 0.0f;

                for (int bin = first_bin; bin <= last_bin; bin++) {
                    strongest = std::max(strongest, frame_data[bin * NUM_HARMONICS + harmonic]);
                }

                energy += strongest;
            }

            mPitchEnergy[frame][static_cast<size_t>(note_idx)] = energy;
        }
    }
}

void NoteVelocity::clear()
{
    mPitchEnergy.clear();
    mPitchEnergy.shrink_to_fit();
}

double NoteVelocity::_attackEnergy(int inStartFrame, int inPitch) const
{
    const int note_idx = inPitch - MIDI_OFFSET;

    if (note_idx < 0 || note_idx >= NUM_FREQ_OUT) {
        return 0.0;
    }

    const auto n_frames = static_cast<int>(mPitchEnergy.size());
    const int first = std::max(0, inStartFrame);

    if (first >= n_frames) {
        return 0.0;
    }

    const int last = std::min(n_frames - 1, first + kAttackFrames);

    double peak = 0.0;

    for (int frame = first; frame <= last; frame++) {
        peak = std::max(peak, static_cast<double>(mPitchEnergy[static_cast<size_t>(frame)][static_cast<size_t>(note_idx)]));
    }

    return peak;
}

void NoteVelocity::apply(std::vector<Notes::Event>& ioEvents) const
{
    if (ioEvents.empty()) {
        return;
    }

    // Without a table there is nothing to measure, but leaving these at zero would be the worst
    // available answer: a MIDI note-on of velocity zero is a note-off to most receivers, so the
    // export would come out silent rather than merely flat. Say "could not measure" uniformly.
    if (mPitchEnergy.empty()) {
        for (auto& event: ioEvents) {
            event.velocity = kNeutralVelocity;
        }

        return;
    }

    std::vector<double> energies;
    energies.reserve(ioEvents.size());

    for (const auto& event: ioEvents) {
        energies.push_back(_attackEnergy(event.startFrame, event.pitch));
    }

    // Normalise against the take's own spread rather than an absolute scale. The feature the model
    // is fed is already a log-power CQT normalised over the whole buffer, so these numbers are
    // proportional to decibels and a linear map onto velocity is the right one: no second log.
    auto sorted = energies;
    std::sort(sorted.begin(), sorted.end());

    const auto percentile = [&sorted](double inShare) {
        const auto position = inShare * static_cast<double>(sorted.size() - 1);
        const auto index = static_cast<size_t>(position + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    };

    const auto median = percentile(0.5);
    const auto span = percentile(kHighPercentile) - percentile(kLowPercentile);

    // A fixed slope, except that a take spanning more than the available velocity range is
    // compressed to fit rather than clipped at both ends. The slope never goes the other way: a
    // take with little variation keeps little variation, which is the whole point.
    const auto available = kMaxVelocity - kMinVelocity;
    const auto slope = span > available / kVelocityPerEnergy ? available / span : kVelocityPerEnergy;

    for (size_t i = 0; i < ioEvents.size(); i++) {
        const auto velocity = kNeutralVelocity + slope * (energies[i] - median);

        ioEvents[i].velocity = std::min(kMaxVelocity, std::max(kMinVelocity, velocity));
    }
}
