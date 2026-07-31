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

    /** Correlation with the winning profile, 0 to 1. Around 0.8 on a clear
        tonal phrase; percussive or atonal material lands near zero, which is
        the signal to distrust the answer rather than hide it.
    */
    float confidence = 0.0f;

    bool isValid() const { return confidence > 0.0f; }

    /** "F# minor". Empty when there was nothing to judge. */
    String toString() const;
};

/**
    Estimate the key with the Krumhansl-Schmuckler profiles.

    Builds a 12-bin pitch-class histogram weighted by how long each note sounds,
    then correlates it against all 24 rotations of the major and minor profiles
    and returns the best. Duration weighting rather than note counting, because
    a held tonic says more about the key than a passing sixteenth.

    No model and no dependency: it runs on note events that already exist.
*/
KeyEstimate estimateKey(const std::vector<Notes::Event>& inNoteEvents);

#endif // KeyEstimate_h
