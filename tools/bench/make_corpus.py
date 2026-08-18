#!/usr/bin/env python3
"""Generate a synthetic piano corpus with exact ground truth for the bench.

This exists so the bench is runnable and verifiable on day one, before anyone has downloaded
MAESTRO. It is a starting point and not a substitute: the notes here are struck by an additive
synth with a clean exponential decay and no pedal, no sympathetic resonance, no room and no
mechanical noise, so scores on it are an upper bound and nothing more. Real material goes in the
same directory alongside it, in the same <name>.wav / <name>.mid pairs.

The cases are chosen to exercise the specific things the decoder was getting wrong:

  scale        a plain reference point, one note at a time
  fast_run     semiquavers at 132 BPM, which the old 125 ms minimum note duration deleted outright
  semitones    minor seconds, which unconditional neighbour suppression made untranscribable
  trill        about 15 notes a second, so the minimum-duration floor has something to lose
  dynamics     one pitch repeated from pianissimo to fortissimo, which velocity-as-confidence
               could not represent at all
  sustained    long overlapping notes, where a fixed offset timeout truncates the decay
  chords       four-note voicings, for polyphony

Usage:  py tools/bench/make_corpus.py <output-dir>
"""

import argparse
import math
import pathlib
import struct
import sys

SAMPLE_RATE = 44100

# Relative amplitude of each partial, and how much faster each one dies away than the
# fundamental. Real strings lose their high partials first, and the model's harmonic stacking
# reads exactly those bins, so getting the ordering right matters more than the exact values.
PARTIALS = [
    (1, 1.00, 1.0),
    (2, 0.50, 1.6),
    (3, 0.28, 2.1),
    (4, 0.16, 2.7),
    (5, 0.09, 3.3),
    (6, 0.05, 4.0),
]


def midi_to_hz(pitch):
    return 440.0 * (2.0 ** ((pitch - 69) / 12.0))


# How fast the damper kills the string once the key comes up, per second. A real damper is not
# instant but it is quick, and this is the difference between a semiquaver that is genuinely short
# and one that rings into its neighbours. Getting this wrong makes a "fast run" case that contains
# no short notes at all, which is worse than having no case.
DAMPER_RATE = 40.0


def render_note(buffer, onset, duration, pitch, velocity):
    """Add one struck-string note to the buffer, in place."""
    start = int(onset * SAMPLE_RATE)

    # Free ring while the key is down, then the damper. Bounded by when the damper has finished.
    length = int((duration + 0.3) * SAMPLE_RATE)
    frequency = midi_to_hz(pitch)
    amplitude = (velocity / 127.0) ** 1.4

    # Lower notes ring longer, as they do on a real instrument.
    decay = 1.6 + 3.0 * max(0.0, (pitch - 21) / 87.0)

    for partial, weight, decay_scale in PARTIALS:
        omega = 2.0 * math.pi * frequency * partial

        if omega >= math.pi * SAMPLE_RATE:  # above Nyquist
            break

        rate = decay * decay_scale
        level = amplitude * weight

        for i in range(length):
            index = start + i
            if index >= len(buffer):
                break

            t = i / SAMPLE_RATE
            envelope = math.exp(-rate * t)

            if t > duration:
                envelope *= math.exp(-DAMPER_RATE * (t - duration))

            if envelope < 1e-4:
                break

            # A short attack ramp, so the onset is a transient rather than a click.
            attack = min(1.0, t / 0.004)
            buffer[index] += level * envelope * attack * math.sin(omega * t)


def write_wav(path, buffer):
    peak = max(abs(v) for v in buffer) or 1.0
    scale = 0.89 / peak  # leave headroom; clipping would be a transient the model can hear

    frames = b"".join(struct.pack("<h", int(max(-32768, min(32767, v * scale * 32767)))) for v in buffer)

    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + len(frames)))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", len(frames)))
        f.write(frames)


def variable_length(value):
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def write_midi(path, notes, ticks_per_quarter=480, tempo_us=500000):
    """Write a type-0 SMF. Hand-rolled to keep the bench free of a MIDI dependency."""
    seconds_per_tick = (tempo_us / 1e6) / ticks_per_quarter

    events = []
    for onset, duration, pitch, velocity in notes:
        events.append((round(onset / seconds_per_tick), 0x90, pitch, velocity))
        events.append((round((onset + duration) / seconds_per_tick), 0x80, pitch, 0))

    # Note-offs before note-ons at the same tick, so a repeated pitch does not read as one note.
    events.sort(key=lambda e: (e[0], e[1]))

    track = bytearray()
    track += b"\x00\xff\x51\x03" + struct.pack(">I", tempo_us)[1:]

    previous = 0
    for tick, status, pitch, velocity in events:
        track += variable_length(tick - previous)
        track += bytes([status, pitch, velocity])
        previous = tick

    track += b"\x00\xff\x2f\x00"

    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, ticks_per_quarter))
        f.write(b"MTrk" + struct.pack(">I", len(track)) + bytes(track))


def case_scale():
    notes = []
    for i, pitch in enumerate([60, 62, 64, 65, 67, 69, 71, 72]):
        notes.append((0.3 + i * 0.5, 0.45, pitch, 80))
    return notes


def case_fast_run():
    # Semiquavers at 132 BPM are 113 ms apart. The old 125 ms floor removed every one of them.
    step = 60.0 / 132.0 / 4.0
    pitches = [60, 62, 64, 65, 67, 69, 71, 72, 71, 69, 67, 65, 64, 62]
    return [(0.3 + i * step, step * 0.9, p, 78) for i, p in enumerate(pitches)]


def case_trill():
    # About 15 notes a second, which is a real trill and not a fast passage. This case exists so
    # the minimum-note-duration floor can actually be swept: without notes shorter than the floor
    # in the corpus, raising the floor only ever removes false positives and the sweep says to
    # raise it forever.
    step = 0.066
    notes = []
    for i in range(24):
        pitch = 71 if i % 2 == 0 else 72
        notes.append((0.4 + i * step, step * 0.85, pitch, 74))
    return notes


def case_semitones():
    notes = []
    for i, root in enumerate([60, 64, 67, 71]):
        onset = 0.3 + i * 0.9
        notes.append((onset, 0.7, root, 82))
        notes.append((onset, 0.7, root + 1, 82))
    return notes


def case_dynamics():
    # One pitch, nothing but loudness changing. Confidence-as-velocity scores near zero here.
    return [(0.3 + i * 0.45, 0.4, 64, v) for i, v in enumerate([20, 34, 48, 62, 76, 90, 104, 118])]


def case_sustained():
    return [
        (0.3, 3.0, 48, 76),
        (0.6, 2.6, 55, 72),
        (0.9, 2.2, 64, 80),
        (1.2, 1.8, 67, 84),
        (3.6, 2.4, 53, 74),
        (3.9, 2.0, 60, 78),
    ]


def case_chords():
    voicings = [[60, 64, 67, 72], [57, 60, 65, 69], [55, 59, 62, 67], [53, 57, 60, 65]]
    notes = []
    for i, voicing in enumerate(voicings):
        onset = 0.3 + i * 1.1
        for j, pitch in enumerate(voicing):
            # Top voice a little louder, as a player would voice it.
            notes.append((onset, 0.95, pitch, 70 + j * 8))
    return notes


CASES = {
    "scale": case_scale,
    "fast_run": case_fast_run,
    "semitones": case_semitones,
    "trill": case_trill,
    "dynamics": case_dynamics,
    "sustained": case_sustained,
    "chords": case_chords,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    arguments = parser.parse_args()

    arguments.output.mkdir(parents=True, exist_ok=True)

    for name, build in CASES.items():
        notes = build()
        end = max(onset + duration for onset, duration, _, _ in notes) + 2.5
        buffer = [0.0] * int(end * SAMPLE_RATE)

        for onset, duration, pitch, velocity in notes:
            render_note(buffer, onset, duration, pitch, velocity)

        write_wav(arguments.output / f"{name}.wav", buffer)
        write_midi(arguments.output / f"{name}.mid", notes)

        print(f"  {name}: {len(notes)} notes, {end:.1f} s")

    print(f"\nwrote {len(CASES)} pairs to {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
