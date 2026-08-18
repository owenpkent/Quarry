//
// Loudness for note events, measured from the audio rather than read off the model.
//

#ifndef NoteVelocity_h
#define NoteVelocity_h

#include <vector>

#include "BasicPitchConstants.h"
#include "Notes.h"

/**
 * Assigns each note event a velocity derived from the audio.
 *
 * Basic Pitch's note posteriorgram is a probability, so passing it to MidiMessage::noteOn -- which
 * is what this project did until now -- encodes how sure the model was in place of how hard the
 * key was struck. On piano that is close to fatal, because voicing, accents and the melody sitting
 * above the accompaniment are all carried by dynamics and nothing else.
 *
 * The measurement reads the harmonic-stacked CQT the feature extractor has already produced and
 * left in memory, so it costs a loop rather than another pass over the audio. For each pitch it
 * sums the fundamental and the first two harmonics, takes the peak over a short window from the
 * onset, and maps the take's own spread of those peaks onto a musical velocity range.
 *
 * Two decisions are worth stating. Harmonics rather than the note's span RMS, because a span's RMS
 * contains every other note sounding at the same time and a quiet note held under a loud chord
 * would read as loud. And the attack rather than the whole note, because a struck string's
 * velocity lives in the transient and everything after it is decay.
 *
 * This sits downstream of note detection and knows nothing about what produced the events, which
 * is deliberate: the transformer tier emits no velocity at all, so it needs exactly this stage and
 * has nothing of its own to patch.
 */
class NoteVelocity
{
public:
    /**
     * Build the per-pitch energy table for a take.
     * @param inStackedCQT Harmonic-stacked CQT, laid out frames x NUM_FREQ_IN x NUM_HARMONICS.
     * @param inNumFrames Number of frames in inStackedCQT.
     */
    void prepare(const float* inStackedCQT, size_t inNumFrames);

    /**
     * Release the energy table.
     */
    void clear();

    bool isPrepared() const { return !mPitchEnergy.empty(); }

    /**
     * Fill in the velocity of every event in place. Without a prepared table every event gets
     * kNeutralVelocity, because a velocity of zero is a note-off to most MIDI receivers and a
     * silent export is a far worse failure than a flat one.
     */
    void apply(std::vector<Notes::Event>& ioEvents) const;

private:
    /** Peak energy over the attack window that follows inStartFrame. */
    double _attackEnergy(int inStartFrame, int inPitch) const;

    // Frames of attack to measure, about 70 ms. Long enough to survive an onset picked a frame or
    // two early, short enough that it is still the strike and not the decay.
    static constexpr int kAttackFrames = 5;

    // Bins either side of the fundamental to search. The CQT runs three bins to the semitone, so
    // this is a third of a semitone of tolerance for a stretched or detuned piano, and it cannot
    // reach into the neighbouring semitone, which is three bins away.
    static constexpr int kBinTolerance = 1;

    // Harmonics of the stacked CQT to sum. Index 1 is the fundamental, 2 the octave, 3 the
    // twelfth. Index 0 is the sub-harmonic and is excluded on purpose: it carries the energy of
    // the note an octave below, which is a different note's dynamics.
    static constexpr int kFirstHarmonic = 1;
    static constexpr int kLastHarmonic = 3;

    // Percentiles used to measure the take's spread. Robust to one clipped strike or one stray
    // quiet detection in a way that min and max are not.
    static constexpr double kLowPercentile = 0.05;
    static constexpr double kHighPercentile = 0.95;

    // Velocity per unit of attack energy.
    //
    // This is a fixed slope on purpose, and an earlier version of this file got it wrong by
    // stretching every take's 5th-to-95th percentile spread onto a fixed velocity range. That
    // amplifies whatever spread happens to be there, so a passage genuinely played at one
    // dynamic comes out with invented dynamics: on the bench's trill, whose reference velocity
    // is constant, the measured energy spread is 0.008 and stretching it to a 0.72 velocity
    // range amplified measurement noise by a factor of about 87.
    //
    // The value is calibrated against the bench case with known full-range dynamics: MIDI 20 to
    // 118 measures as an energy spread of about 1.3, and that range is 0.77 in these units.
    static constexpr double kVelocityPerEnergy = 0.6;

    // Velocity the median note of any take is placed at. Absolute loudness is not recoverable
    // from a recording of unknown gain, so only the spread around this carries information.
    static constexpr double kNeutralVelocity = 0.63;

    static constexpr double kMinVelocity = 0.16;
    static constexpr double kMaxVelocity = 1.0;

    // [frame][note index], where note index is MIDI pitch minus MIDI_OFFSET.
    std::vector<std::vector<float>> mPitchEnergy;
};

#endif // NoteVelocity_h
