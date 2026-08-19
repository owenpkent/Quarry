"""Shared mir_eval scoring for the Quarry bake-off.

Both crosscheck_mir_eval.py (does the bench's own scorer agree with mir_eval, the reference
implementation used across the wider transcription literature) and run_bakeoff.py (does every
candidate engine land on the same yardstick) need the identical notion of "how do these two note
sets score against each other." This is that notion, written once.

Note-level matching is mir_eval.transcription.match_notes: same pitch, onset within tolerance,
and (for the onset+offset variant) offset within tolerance too. Aggregation across cases sums
matched/estimated/reference counts and computes precision/recall/F1 from the sums, not from the
per-case F1 values, so a long difficult case is not outvoted by a short easy one. This mirrors
how tools/bench/Bench.cpp itself aggregates.
"""

from dataclasses import dataclass

import mir_eval.transcription as mir_eval_transcription
import numpy as np
import pretty_midi

# mir_eval's own default and the standard in the literature: see kOnsetToleranceSeconds in
# tools/bench/Bench.cpp.
ONSET_TOLERANCE_SECONDS = 0.05

# Matches kOffsetToleranceRatio in Bench.cpp: the larger of the onset tolerance and this share of
# the reference note's own duration.
OFFSET_RATIO = 0.2


@dataclass
class Counts:
    """Matched/estimated/reference note counts, summable across cases before deriving P/R/F1."""

    matched: int
    estimated: int
    reference: int

    def __add__(self, other: "Counts") -> "Counts":
        return Counts(self.matched + other.matched, self.estimated + other.estimated,
                      self.reference + other.reference)

    @property
    def precision(self) -> float:
        return self.matched / self.estimated if self.estimated > 0 else 0.0

    @property
    def recall(self) -> float:
        return self.matched / self.reference if self.reference > 0 else 0.0

    @property
    def f1(self) -> float:
        p, r = self.precision, self.recall
        return 2.0 * p * r / (p + r) if (p + r) > 0.0 else 0.0


def midi_pitch_to_hz(pitch) -> np.ndarray:
    """mir_eval wants pitches in Hertz; everything else here (MIDI, the bench's TSVs) is in MIDI
    note numbers."""
    return 440.0 * (2.0 ** ((np.asarray(pitch, dtype=float) - 69.0) / 12.0))


def load_notes_from_midi(path) -> tuple[np.ndarray, np.ndarray]:
    """Read all notes across all instruments of a MIDI file as (intervals, pitches_hz)."""
    pm = pretty_midi.PrettyMIDI(str(path))

    onsets, offsets, pitches = [], [], []

    for instrument in pm.instruments:
        for note in instrument.notes:
            onsets.append(note.start)
            # A zero-or-negative-length note is a malformed file, not a note of zero duration.
            offsets.append(note.end if note.end > note.start else note.start)
            pitches.append(note.pitch)

    intervals = np.array(list(zip(onsets, offsets)), dtype=float).reshape(-1, 2)
    return intervals, midi_pitch_to_hz(pitches)


def load_notes_from_tsv(path) -> tuple[np.ndarray, np.ndarray]:
    """Read a Bench.exe --dump-notes file: header onset_s/offset_s/pitch/velocity, one note per
    line, pitch as a MIDI note number, as (intervals, pitches_hz)."""
    onsets, offsets, pitches = [], [], []

    with open(path, "r", encoding="utf-8") as handle:
        next(handle, None)  # header line

        for raw in handle:
            line = raw.strip()

            if not line:
                continue

            onset_s, offset_s, pitch, _velocity = line.split("\t")
            onsets.append(float(onset_s))
            offsets.append(float(offset_s))
            pitches.append(float(pitch))

    intervals = np.array(list(zip(onsets, offsets)), dtype=float).reshape(-1, 2)
    return intervals, midi_pitch_to_hz(pitches)


def write_notes_midi(path, notes) -> None:
    """Write (onset_s, offset_s, pitch, velocity) tuples out as a single-instrument MIDI file, so
    every engine's output ends up in the same format run_bakeoff.py scores from."""
    pm = pretty_midi.PrettyMIDI()
    instrument = pretty_midi.Instrument(program=0, name="transcription")

    for onset, offset, pitch, velocity in notes:
        velocity_int = int(round(min(max(velocity, 1.0), 127.0)))
        end = offset if offset > onset else onset + 0.001
        instrument.notes.append(pretty_midi.Note(velocity=velocity_int, pitch=int(round(pitch)),
                                                  start=onset, end=end))

    pm.instruments.append(instrument)
    pm.write(str(path))


def score_case(ref_intervals: np.ndarray, ref_pitches: np.ndarray, est_intervals: np.ndarray,
              est_pitches: np.ndarray) -> tuple[Counts, Counts]:
    """Score one case both ways: onset-only, and onset-and-offset. Returns (onset, onset_offset)."""
    n_ref = ref_intervals.shape[0]
    n_est = est_intervals.shape[0]

    if n_ref == 0 or n_est == 0:
        empty = Counts(0, n_est, n_ref)
        return empty, empty

    onset_matching = mir_eval_transcription.match_notes(ref_intervals, ref_pitches, est_intervals,
                                                        est_pitches, onset_tolerance=ONSET_TOLERANCE_SECONDS,
                                                        offset_ratio=None)
    onset_offset_matching = mir_eval_transcription.match_notes(ref_intervals, ref_pitches, est_intervals,
                                                                est_pitches, onset_tolerance=ONSET_TOLERANCE_SECONDS,
                                                                offset_ratio=OFFSET_RATIO,
                                                                offset_min_tolerance=ONSET_TOLERANCE_SECONDS)

    return Counts(len(onset_matching), n_est, n_ref), Counts(len(onset_offset_matching), n_est, n_ref)


def print_table(rows: list[tuple[str, Counts, Counts]], onset_total: Counts, onset_offset_total: Counts) -> None:
    """Print a table in the same spirit as Bench.cpp's own: one line per case, a rule, then ALL."""
    header = f"{'item':<20}{'P':>8}{'R':>8}{'F1':>8}{'+off F1':>9}{'ref':>7}{'est':>7}"
    print()
    print(header)
    print("-" * len(header))

    for name, onset, onset_offset in rows:
        print(f"{name:<20}{onset.precision:8.3f}{onset.recall:8.3f}{onset.f1:8.3f}{onset_offset.f1:9.3f}"
              f"{onset.reference:7d}{onset.estimated:7d}")

    print("-" * len(header))
    print(f"{'ALL':<20}{onset_total.precision:8.3f}{onset_total.recall:8.3f}{onset_total.f1:8.3f}"
          f"{onset_offset_total.f1:9.3f}{onset_total.reference:7d}{onset_total.estimated:7d}")
    print()
    print(f"mir_eval.transcription.match_notes: onset tolerance {ONSET_TOLERANCE_SECONDS * 1000:.0f} ms; "
          f"offset tolerance max(that, {OFFSET_RATIO} x ref note length).")
    print("aggregate is summed matched/estimated/reference counts across cases, not averaged per-case F1.")
