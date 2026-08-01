//
// Working out what key a transcription is in.
//

#ifndef KeyEstimate_h
#define KeyEstimate_h

#include "JuceHeader.h"

#include "Notes.h"

/**
    The key a set of transcribed notes appears to be in.

    This is a reading of the notes, not an instruction to them. Scale quantize
    tells Quarry what to snap to; this says what it heard, so the two are shown
    apart and only ever meet when the user asks.
*/
struct KeyEstimate {
    /** 0 = C, 1 = C sharp, through to 11 = B. */
    int rootNote = 0;

    bool isMinor = false;

    /** How sure we are: the correlation with the winning profile, clamped to 0 to 1.

        This is a Krumhansl correlation, not a probability, and it does not fall
        away to zero on material with no key. The best of the twenty-four
        rotations is positive for any histogram that is not perfectly flat, and a
        sparse one can score higher than a real scale does. Weak material is
        rejected by the pitch-class support gate in estimateKey and by the
        threshold below, never by this number drifting towards zero on its own.
    */
    float confidence = 0.0f;

    /** Measured phrases run from 0.76 for a flat major scale up to 0.98 for one
        that leans on its tonic, so half sits clear of anything genuinely tonal
        while still discarding a muddled reading.
    */
    static constexpr float kMinConfidence = 0.5f;

    bool isValid() const { return confidence >= kMinConfidence; }

    /** "F# minor". Empty when there was nothing to judge. */
    String toString() const;
};

/**
    Estimate the key with the Krumhansl-Schmuckler profiles.

    Builds a 12-bin pitch-class histogram weighted by how long each note sounds,
    then correlates it against all 24 rotations of the major and minor profiles
    and returns the best. Duration weighting rather than note counting, because
    a held tonic says more about the key than a passing sixteenth.

    Returns a default, invalid estimate when there is nothing to read: an empty
    take, notes carrying no weight, or a histogram whose energy sits in too few
    pitch classes to be a key at all.

    No model and no dependency: it runs on note events that already exist.
*/
KeyEstimate estimateKey(const std::vector<Notes::Event>& inNoteEvents);

#endif // KeyEstimate_h
