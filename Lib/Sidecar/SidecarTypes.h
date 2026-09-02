//
// Plain data the sidecar protocol produces, split out of SidecarClient.h so a caller that only
// wants the shapes (e.g. MidiFileWriter, threading pedal events through) does not have to pull in
// SidecarClient's own platform process-handling headers (windows.h/unistd.h) along with it.
//

#ifndef SidecarTypes_h
#define SidecarTypes_h

#include <JuceHeader.h>

/**
 * One note as reported by the sidecar, in its own units: seconds and a 0-127 MIDI-style
 * velocity. velocity is -1 when the sidecar reported it as JSON null.
 */
struct SidecarNote {
    double onset = 0.0;
    double offset = 0.0;
    int pitch = 0;
    int velocity = -1;
};

/** One sustain-pedal sample as reported by the sidecar. */
struct SidecarPedalEvent {
    double time = 0.0;
    int value = 0;
};

/**
 * One progress line reported by the sidecar while a request is in flight (protocol version 2's
 * "stage" event; see tools/sidecar/PROTOCOL.md). fraction is -1.0 when the sidecar did not report
 * one -- not every stage knows a percentage, and 0.0 already means something (just started).
 */
struct SidecarStage {
    juce::String stage;
    juce::String text;
    double t = 0.0;
    double fraction = -1.0;
};

#endif // SidecarTypes_h
