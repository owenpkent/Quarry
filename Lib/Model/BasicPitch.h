//
// Created by Damien Ronssin on 10.03.23.
//

#ifndef BasicPitch_h
#define BasicPitch_h

#include "BasicPitchCNN.h"
#include "BasicPitchConstants.h"
#include "Features.h"
#include "NoteVelocity.h"
#include "Notes.h"

/**
 * Class to get midi transcription from raw audio.
 */
class BasicPitch
{
public:
    BasicPitch() = default;

    /**
     * Resets all states of model, clear the posteriorgrams vector computed by the CNN and the note event vector.
     */
    void reset();

    /**
     * Set parameters for next transcription or midi update.
     *
     * Both sensitivities are offsets from a threshold derived from the take itself, not absolute
     * values: 0.5 is neutral and means "use what the audio says". See _deriveThresholds.
     *
     * @param inNoteSensitivity Note sensitivity (0.05, 0.95), 0.5 neutral. Higher gives more notes.
     * @param inSplitSensitivity Split sensitivity (0.05, 0.95), 0.5 neutral. Higher will split note more, lower will merge close notes with same pitch
     * @param inMinNoteDurationMs Minimum note duration to keep in ms.
     */
    void setParameters(float inNoteSensitivity, float inSplitSensitivity, float inMinNoteDurationMs);

    /**
     * Reproduce the engine as it stood before the fixes in ANALYSIS.md §2: basic-pitch's raw
     * 1 - knob thresholds, its fixed offset timeout, unconditional neighbour suppression, and
     * onsets left on the frame grid.
     *
     * This exists for the bench. Without an A/B switch a change in the numbers cannot be
     * attributed to the fixes rather than to the corpus, and an unattributable number is not
     * evidence. Nothing in the plugin sets this.
     */
    void setLegacyEngine(bool inLegacy);

    /**
     * Transcribe the input audio. The note event vector can be obtained after this with getNoteEvents
     * @param inAudio Pointer to raw audio (must be at 22050 Hz)
     * @param inNumSamples Number of input samples available.
     */
    void transcribeToMIDI(float* inAudio, int inNumSamples);

    /**
     * Function to call to update the midi transcription with new parameters.
     * The whole Features + CNN is not rerun for this. Only Notes::Convert is.
     */
    void updateMIDI();

    /**
     * @return Note event vector.
     */
    const std::vector<Notes::Event>& getNoteEvents() const;

private:
    /**
     * Read the two thresholds that matter most off the take's own posteriorgram distributions.
     *
     * They were previously handed to the user raw, as 1 - knob, with no reference to the material.
     * Published work on Basic Pitch finds moving the frame threshold from 0.5 to 0.6 alone worth
     * close to 50% relative F1 on one corpus, which is a larger effect than any single fix in the
     * decoder, and asking someone to find that by ear with no feedback is not a design.
     */
    void _deriveThresholds();

    /**
     * Fold the stored knob positions into the derived thresholds and into mParams.
     */
    void _applyParameters();

    // Posteriorgrams vector
    std::vector<std::vector<float>> mContoursPG;
    std::vector<std::vector<float>> mNotesPG;
    std::vector<std::vector<float>> mOnsetsPG;

    std::vector<Notes::Event> mNoteEvents;

    Notes::ConvertParams mParams;

    // Knob positions as given, kept so the derived thresholds can be re-folded once a take has
    // been analysed without the caller having to set them again.
    float mNoteSensitivity = 0.5f;
    float mSplitSensitivity = 0.5f;
    float mMinNoteDurationMs = 75.0f;

    bool mLegacyEngine = false;

    // Thresholds read off the take. Until there is a take to read they hold basic-pitch's own
    // defaults, which is exactly where the centred knobs used to put them.
    float mDerivedFrameThreshold = 0.3f;
    float mDerivedOnsetThreshold = 0.5f;

    // Where this take's note posteriorgram stops being silence. Bounds how far a note is allowed
    // to ring on past its core before it is called finished.
    float mDerivedNoiseFloor = 0.05f;

    size_t mNumFrames = 0;

    Features mFeaturesCalculator;
    BasicPitchCNN mBasicPitchCNN;
    Notes mNotesCreator;
    NoteVelocity mNoteVelocity;
};

#endif // BasicPitch_h
