//
// Pure-logic coverage for the Quarry-side sidecar integration: the two pieces that do not need
// audio or a running sidecar process to test.
//
//  - resolveSidecarActivation: QUARRY_SIDECAR_CMD / QUARRY_SIDECAR_ENGINE parsing.
//  - MidiFileWriter's pedal-tick conversion: that CC64 events land on the same wall-clock-to-tick
//    formula as note events, that an empty pedal list writes none, and that the values survive
//    the round trip through a written-then-read-back MIDI file.
//
// Everything else about the sidecar path (starting the process, the transcribe request/response,
// the NoteVelocity gate) needs a running sidecar or a full plugin instance and is exercised by
// tools/bench's --sidecar flag instead.
//

#ifndef NN_SIDECAR_INTEGRATION_TEST_H
#define NN_SIDECAR_INTEGRATION_TEST_H

#include <cmath>
#include <iostream>

#include <JuceHeader.h>

#include "MidiFile/MidiFileWriter.h"
#include "SidecarActivation.h"
#include "TimeQuantizeOptions.h"

namespace sidecar_integration_test_detail
{
inline bool expect(bool inCondition, const char* inWhat, bool& ioOk)
{
    if (!inCondition) {
        std::cout << "FAIL: " << inWhat << std::endl;
        ioOk = false;
    }

    return inCondition;
}

inline bool testActivationParsing()
{
    bool ok = true;

    const auto unset = resolveSidecarActivation("", "");
    expect(!unset.active, "empty QUARRY_SIDECAR_CMD leaves the sidecar tier inactive", ok);

    const auto whitespace_only = resolveSidecarActivation("   ", "kong");
    expect(!whitespace_only.active, "whitespace-only QUARRY_SIDECAR_CMD is treated as unset", ok);

    const auto default_engine = resolveSidecarActivation("py quarry_sidecar.py serve", "");
    expect(default_engine.active, "a non-empty QUARRY_SIDECAR_CMD activates the sidecar tier", ok);
    expect(default_engine.commandLine == "py quarry_sidecar.py serve",
          "the command line is carried through unchanged",
          ok);
    expect(default_engine.engine == "auto", "an unset QUARRY_SIDECAR_ENGINE defaults to \"auto\"", ok);

    const auto explicit_engine = resolveSidecarActivation("py quarry_sidecar.py serve", "transkun");
    expect(explicit_engine.engine == "transkun", "QUARRY_SIDECAR_ENGINE overrides the \"auto\" default", ok);

    return ok;
}

inline bool testPedalTickConversion()
{
    bool ok = true;

    // A default TimeQuantizeInfo (120 bpm, 4/4, zero reference position) has a zero last-bar-start
    // offset, so the tick formula below can be checked directly against MidiFileWriter's own
    // (time + start_offset) * bpm / 60 * ticksPerQuarterNote without also re-deriving start_offset.
    const TimeQuantizeOptions::TimeQuantizeInfo info;
    expect(info.getStartLastBarSec() == 0.0, "default TimeQuantizeInfo has a zero bar offset", ok);

    std::vector<Notes::Event> notes(1);
    notes[0].startTime = 0.0;
    notes[0].endTime = 1.0;
    notes[0].pitch = 60;
    notes[0].velocity = 0.8;

    const double export_bpm = 120.0;
    const int ticks_per_quarter_note = 960; // MidiFileWriter's own mTicksPerQuarterNote.

    const MidiFileWriter writer;

    // Empty pedal list: the writer's output for a no-pedal transcription must be unchanged.
    {
        const auto temp_file = juce::File::createTempFile(".mid");
        const auto written = writer.writeMidiFile(notes, temp_file, info, export_bpm, NoPitchBend, {});
        expect(written, "writeMidiFile succeeds with no pedal events", ok);

        juce::FileInputStream stream(temp_file);
        juce::MidiFile midi;
        midi.readFrom(stream);

        bool found_controller = false;
        const auto& track = *midi.getTrack(0);

        for (int i = 0; i < track.getNumEvents(); i++) {
            if (track.getEventPointer(i)->message.isController()) {
                found_controller = true;
            }
        }

        expect(!found_controller, "an empty pedal list writes no CC64 events", ok);

        temp_file.deleteFile();
    }

    // Non-empty pedal list: CC64 on channel 1, at the same tick conversion as note events.
    {
        const std::vector<SidecarPedalEvent> pedal = {{0.5, 127}, {0.9, 0}};

        const auto temp_file = juce::File::createTempFile(".mid");
        const auto written = writer.writeMidiFile(notes, temp_file, info, export_bpm, NoPitchBend, pedal);
        expect(written, "writeMidiFile succeeds with pedal events", ok);

        juce::FileInputStream stream(temp_file);
        juce::MidiFile midi;
        midi.readFrom(stream);

        std::vector<std::pair<int, double>> controller_events; // (value, tick)
        const auto& track = *midi.getTrack(0);

        for (int i = 0; i < track.getNumEvents(); i++) {
            const auto& message = track.getEventPointer(i)->message;

            if (message.isController() && message.getControllerNumber() == 64) {
                expect(message.getChannel() == 1, "CC64 is written on channel 1", ok);
                controller_events.emplace_back(message.getControllerValue(), message.getTimeStamp());
            }
        }

        if (expect(controller_events.size() == 2, "both pedal events came back as CC64", ok)) {
            const double expected_on_tick = 0.5 * export_bpm / 60.0 * ticks_per_quarter_note;
            const double expected_off_tick = 0.9 * export_bpm / 60.0 * ticks_per_quarter_note;

            expect(controller_events[0].first == 127, "pedal-down value (127) survives the round trip", ok);
            expect(std::abs(controller_events[0].second - expected_on_tick) < 1.0,
                  "pedal-down tick matches the note tick formula",
                  ok);

            expect(controller_events[1].first == 0, "pedal-up value (0) survives the round trip", ok);
            expect(std::abs(controller_events[1].second - expected_off_tick) < 1.0,
                  "pedal-up tick matches the note tick formula",
                  ok);
        }

        temp_file.deleteFile();
    }

    return ok;
}
} // namespace sidecar_integration_test_detail

inline bool sidecar_integration_test()
{
    bool ok = true;

    ok &= sidecar_integration_test_detail::testActivationParsing();
    ok &= sidecar_integration_test_detail::testPedalTickConversion();

    return ok;
}

#endif // NN_SIDECAR_INTEGRATION_TEST_H
