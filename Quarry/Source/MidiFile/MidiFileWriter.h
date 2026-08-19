//
// Created by Damien Ronssin on 11.03.23.
//

#ifndef MidiFileWriter_h
#define MidiFileWriter_h

#include <JuceHeader.h>

#include "BasicPitchConstants.h"
#include "Notes.h"
#include "SidecarTypes.h"
#include "TimeQuantizeOptions.h"

class MidiFileWriter
{
public:
    /**
     * @param inPedalEvents Sustain-pedal (CC64) events, from the sidecar, in the same wall-clock
     *  seconds as inNoteEvents' timestamps. Written on channel 1 with the same tick conversion as
     *  the notes. Defaulted so existing callers keep compiling; an empty list (the default, and
     *  what every non-sidecar take passes) writes no CC64 at all, so the file is unchanged from
     *  before this parameter existed.
     *
     *  Time quantization, when enabled, only ever touches inNoteEvents: pedal always maps its own
     *  unquantized wall-clock time straight to ticks. Making pedal follow the note grid too is
     *  future work.
     */
    bool writeMidiFile(const std::vector<Notes::Event>& inNoteEvents,
                       const File& fileToUse,
                       const TimeQuantizeOptions::TimeQuantizeInfo& inInfo,
                       double inExportBpm,
                       PitchBendModes inPitchBendMode,
                       const std::vector<SidecarPedalEvent>& inPedalEvents = {}) const;

private:
    static double _BPMToMicrosecondsPerQuarterNote(double inTempoBPM);

    const int mTicksPerQuarterNote = 960;
};

#endif // MidiFileWriter_h
