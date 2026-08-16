//
// Created by Damien Ronssin on 13.07.2024.
//

#ifndef NNID_H
#define NNID_H

#include <JuceHeader.h>

namespace NnId
{
inline static Identifier ValueId = "value";

inline static Identifier IdId = "id";

// Still NEURAL_NOTE: an opaque ValueTree tag written into saved host sessions. Renaming it
// would drop the state of every session saved before the rename to Quarry, and buy nothing.
inline static Identifier FullStateId = "NEURAL_NOTE_FULL_STATE";

inline static Identifier ParametersId = "PARAMETERS";

// Still NEURAL_NOTE, for the same reason as FullStateId above.
inline static Identifier QuarryStateId = "NEURAL_NOTE_STATE";

// Still NEURAL_NOTE, for the same reason as FullStateId above.
inline static Identifier QuarryVersionId = "NEURAL_NOTE_VERSION";

inline static Identifier SourceAudioNativeSrPathId = "SOURCE_AUDIO_NATIVE_SR_PATH";

inline static Identifier PlayheadPositionSecId = "PLAYHEAD_POSITION_SEC";

inline static Identifier PlayheadCenteredId = "PLAYHEAD_CENTERED";

inline static Identifier MidiOut = "MIDI_OUT";

inline static Identifier ExportTempoId = "EXPORT_TEMPO";

inline static Identifier ZoomLevelId = "ZOOM_LEVEL";

inline static Identifier TooltipVisibleId = "TOOLTIP_VISIBLE";

// --------------- Keeping takes ----------------
// Where Save writes, and in which formats. Project state rather than a machine
// preference: a session's samples belong with the session, so a project opened
// on another day writes to the same place.
inline static Identifier SampleFolderId = "SAMPLE_FOLDER";

inline static Identifier SampleWriteWavId = "SAMPLE_WRITE_WAV";

inline static Identifier SampleWriteMidiId = "SAMPLE_WRITE_MIDI";

// Where the Sample page writes its captures. Deliberately not SampleFolderId: that one
// holds finished takes you chose to keep, this one holds raw captures off other
// applications, and a folder carrying both is harder to browse than either alone.
inline static Identifier CaptureFolderId = "CAPTURE_FOLDER";

// --------------- Time quantization ----------------
inline static Identifier TempoId = "TEMPO";

inline static Identifier TimeSignatureNumeratorId = "TIME_SIGNATURE_NUMERATOR";

inline static Identifier TimeSignatureDenominatorId = "TIME_SIGNATURE_DENOMINATOR";

inline static Identifier TimeQuantizeRefPosQnId = "TIME_QUANTIZE_REF_POS_QN";

inline static Identifier TimeQuantizeRefLastBarQnId = "TIME_QUANTIZE_REF_LAST_BAR_QN";

inline static Identifier TimeQuantizeRefPosSec = "TIME_QUANTIZE_REF_POS_SECONDS";

// To be set in this specific order
const std::vector<std::pair<Identifier, var>> OrderedStatePropertiesWithDefault = {
    {TempoId, 120.0},
    {ExportTempoId, 120.0},
    {TimeSignatureNumeratorId, 4},
    {TimeSignatureDenominatorId, 4},
    {TimeQuantizeRefPosQnId, 0.0},
    {TimeQuantizeRefLastBarQnId, 0.0},
    {TimeQuantizeRefPosSec, 0.0},
    {SourceAudioNativeSrPathId, String()},
    {PlayheadPositionSecId, 0.0},
    {PlayheadCenteredId, true},
    {ZoomLevelId, 1.0},
    {MidiOut, false},
    {TooltipVisibleId, true},
    {SampleFolderId, String()},
    {SampleWriteWavId, true},
    {SampleWriteMidiId, true},
    {CaptureFolderId, String()}};

} // namespace NnId

#endif //NNID_H
