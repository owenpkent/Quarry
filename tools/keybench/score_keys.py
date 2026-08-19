#!/usr/bin/env python3
"""Score key-profile sets against a labelled corpus, for the key-detection bench.

Replicates Lib/Model/KeyEstimate.cpp's histogram exactly -- duration times velocity weighting,
the same 3-pitch-class / 1%-share support gate -- so an improvement measured here means an
improvement there. This is a from-scratch Python re-implementation kept faithful to the C++ by
inspection (see KeyEstimate.cpp around line 103 for the weighting, and kSupportShare /
kMinSupportingPitchClasses for the guards); it never calls into the C++ and nothing here modifies
it. There is no automated parity check yet, so re-read both files by eye whenever KeyEstimate.cpp
changes.

Deliberately NOT replicated: KeyEstimate::kMinConfidence (0.5), the "is this answer worth
trusting" cutoff KeyEstimate applies to its own output. That threshold was calibrated against the
Krumhansl-Kessler correlation's own score range (0.76 to 0.98 on measured phrases, per
KeyEstimate.h) and has no reason to mean the same thing on Temperley-Kostka-Payne, Albrecht-
Shanahan, or the binary modal templates below, whose correlations live on different scales. The
pitch-class support gate is a property of the histogram, not of any one profile set, so it is
applied uniformly to all four before any of them get a vote; every track that clears it gets a
best-guess answer from every profile set, which is what accuracy/MIREX scoring needs.

Input: a directory of per-track note files, a labels TSV (as written by fetch_giantsteps.py:
track_id, tonic, mode). Each track_id is looked up as <dir>/<track_id>.est.tsv (Bench.exe
--dump-notes' own naming), <dir>/<track_id>.tsv, <dir>/<track_id>.mid, or <dir>/<track_id>.midi,
in that order. TSVs need the header onset_s/offset_s/pitch/velocity; MIDI is read with a small
hand-rolled SMF parser (metrical time division only, matching make_corpus.py's hand-rolled writer)
so this stays free of a MIDI dependency, same as the rest of tools/bench.

Usage:
  py tools/keybench/score_keys.py <notes-dir> <labels.tsv>
  py tools/keybench/score_keys.py --self-test
"""

import argparse
import csv
import pathlib
import random
import struct
import sys
from collections import Counter

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
NOTE_INDEX = {name: i for i, name in enumerate(NOTE_NAMES)}

# --- Krumhansl-Kessler ------------------------------------------------------------------------
# Copied verbatim from Lib/Model/KeyEstimate.cpp (kMajorProfile / kMinorProfile).
KK_MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
KK_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]

# --- Temperley-Kostka-Payne -------------------------------------------------------------------
# Temperley, D. (2001), "The Cognition of Basic Musical Structures", MIT Press: corpus counts
# over the Kostka-Payne common-practice theory-textbook excerpts. Values cross-checked against
# music21's analysis.discrete.TemperleyKostkaPayne, which reproduces the same table.
TKP_MAJOR = [0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400]
TKP_MINOR = [0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330]

# --- Albrecht-Shanahan -------------------------------------------------------------------------
# Albrecht, J. D. & Shanahan, D. (2013), "The Use of Large Corpora to Train a New Type of
# Key-Finding Algorithm: An Improved Treatment of the Minor Mode", Music Perception 31(1), 59-67.
# Values cross-checked against an independent open-source reproduction of the same table
# (shanahdt/huji_summer_class_2026, utils/keyfinding.py -- Daniel Shanahan's own teaching repo).
AS_MAJOR = [0.238, 0.006, 0.111, 0.006, 0.137, 0.094, 0.016, 0.214, 0.009, 0.080, 0.008, 0.081]
AS_MINOR = [0.220, 0.006, 0.104, 0.123, 0.019, 0.103, 0.012, 0.214, 0.062, 0.022, 0.061, 0.052]

# --- Modal set: the seven diatonic modes as binary in-scale templates -------------------------
# Semitone offsets of each mode's scale degrees from its own tonic. A pitch class scores 1 if it
# is in the mode's scale, 0 otherwise; the tonic itself is doubled to 2. Without that doubling,
# every mode built on the same seven-note collection (e.g. C Ionian and A Aeolian, which share
# every pitch class) would be indistinguishable by Pearson correlation -- see the "structural
# limits" section of --self-test for a worked proof.
MODE_DEGREES = {
    "Ionian": [0, 2, 4, 5, 7, 9, 11],
    "Dorian": [0, 2, 3, 5, 7, 9, 10],
    "Phrygian": [0, 1, 3, 5, 7, 8, 10],
    "Lydian": [0, 2, 4, 6, 7, 9, 11],
    "Mixolydian": [0, 2, 4, 5, 7, 9, 10],
    "Aeolian": [0, 2, 3, 5, 7, 8, 10],
    "Locrian": [0, 1, 3, 5, 6, 8, 10],
}

# Ionian is the major scale and Aeolian is the natural minor scale -- not an approximation, the
# same seven-note pattern under a different name -- so these two are the only modal predictions
# that can ever be scored "correct" against a major/minor ground-truth label. A prediction landing
# on one of the other five modes is tallied separately (category "other-mode") rather than forced
# into a major/minor bucket it doesn't belong in.
MODE_TO_GROUND_TRUTH = {"Ionian": "major", "Aeolian": "minor"}


def make_template(degrees, tonic_weight):
    template = [0.0] * 12
    for degree in degrees:
        template[degree] = 1.0
    template[0] = tonic_weight
    return template


MODAL_TEMPLATES = {name: make_template(degrees, 2.0) for name, degrees in MODE_DEGREES.items()}
PLAIN_TEMPLATES = {name: make_template(degrees, 1.0) for name, degrees in MODE_DEGREES.items()}

PROFILE_PAIRS = {
    "kk": (KK_MAJOR, KK_MINOR),
    "tkp": (TKP_MAJOR, TKP_MINOR),
    "as": (AS_MAJOR, AS_MINOR),
}

PROFILE_LABELS = [
    ("kk", "Krumhansl-Kessler"),
    ("tkp", "Temperley-Kostka-Payne"),
    ("as", "Albrecht-Shanahan"),
    ("modal", "Modal (7-mode, doubled tonic)"),
]

# Same support gate as KeyEstimate.cpp: kSupportShare and kMinSupportingPitchClasses.
SUPPORT_SHARE = 0.01
MIN_SUPPORTING_PITCH_CLASSES = 3


def correlate(histogram, profile, root):
    """Pearson correlation of histogram (rotated to root) against profile. Direct port of
    KeyEstimate.cpp's anonymous-namespace correlate()."""
    hist_mean = sum(histogram) / 12.0
    prof_mean = sum(profile) / 12.0

    numerator = 0.0
    hist_var = 0.0
    prof_var = 0.0

    for i in range(12):
        h = histogram[(i + root) % 12] - hist_mean
        p = profile[i] - prof_mean
        numerator += h * p
        hist_var += h * h
        prof_var += p * p

    denominator = (hist_var * prof_var) ** 0.5
    if denominator <= 0.0:
        return 0.0
    return numerator / denominator


def passes_support_gate(histogram):
    total_weight = sum(histogram)
    if total_weight <= 0.0:
        return False
    supporting = sum(1 for b in histogram if b >= SUPPORT_SHARE * total_weight)
    return supporting >= MIN_SUPPORTING_PITCH_CLASSES


def build_histogram(events):
    """events: iterable of (onset_s, offset_s, pitch, velocity). Returns a 12-bin histogram, or
    None if the take is empty, carries no weight, or fails the pitch-class support gate -- the
    same three ways KeyEstimate::estimateKey returns its default, invalid estimate."""
    histogram = [0.0] * 12
    total_weight = 0.0

    for onset, offset, pitch, velocity in events:
        duration = offset - onset
        if duration <= 0.0 or pitch < 0:
            continue
        weight = duration * max(0.0, velocity)
        histogram[int(pitch) % 12] += weight
        total_weight += weight

    if total_weight <= 0.0:
        return None
    if not passes_support_gate(histogram):
        return None
    return histogram


def best_kkt_style(histogram, major, minor):
    """Best (tonic, mode, correlation) over 24 rotations of a major/minor profile pair, exactly
    as KeyEstimate::estimateKey searches Krumhansl-Kessler."""
    best_score, best_root, best_mode = -1.0, 0, "major"
    for root in range(12):
        c_major = correlate(histogram, major, root)
        c_minor = correlate(histogram, minor, root)
        if c_major > best_score:
            best_score, best_root, best_mode = c_major, root, "major"
        if c_minor > best_score:
            best_score, best_root, best_mode = c_minor, root, "minor"
    return best_root, best_mode, best_score


def best_modal(histogram, templates=None):
    """Best (tonic, mode-name, correlation) over 7 modes x 12 rotations = 84 candidates."""
    templates = templates or MODAL_TEMPLATES
    best_score, best_root, best_name = -1.0, 0, "Ionian"
    for root in range(12):
        for name, template in templates.items():
            c = correlate(histogram, template, root)
            if c > best_score:
                best_score, best_root, best_name = c, root, name
    return best_root, best_name, best_score


def score_profile_set(histogram, kind):
    if kind == "modal":
        return best_modal(histogram)
    major, minor = PROFILE_PAIRS[kind]
    return best_kkt_style(histogram, major, minor)


# --- MIREX weighted key score -------------------------------------------------------------------

def classify(ref_tonic, ref_mode, est_tonic, est_mode_raw):
    """(category, score) for one estimate against one reference, MIREX-style: correct 1.0, perfect
    fifth 0.5, relative major/minor 0.3, parallel major/minor 0.2, else other 0.0."""
    est_mode = MODE_TO_GROUND_TRUTH.get(est_mode_raw, est_mode_raw)

    if est_mode not in ("major", "minor"):
        return "other-mode", 0.0  # modal set landed on Dorian/Phrygian/Lydian/Mixolydian/Locrian

    if est_tonic == ref_tonic and est_mode == ref_mode:
        return "correct", 1.0

    if est_mode == ref_mode and (est_tonic - ref_tonic) % 12 in (5, 7):
        return "fifth", 0.5

    if est_mode != ref_mode:
        if ref_mode == "major" and (ref_tonic - est_tonic) % 12 == 3:
            return "relative", 0.3
        if ref_mode == "minor" and (est_tonic - ref_tonic) % 12 == 3:
            return "relative", 0.3

    if est_tonic == ref_tonic and est_mode != ref_mode:
        return "parallel", 0.2

    return "other", 0.0


# --- Note-file loading ---------------------------------------------------------------------------

def load_notes_tsv(path):
    events = []
    with open(path, encoding="utf-8", newline="") as f:
        reader = csv.reader(f, delimiter="\t")
        header = next(reader, None)
        if header is None:
            return events
        if [h.strip().lower() for h in header[:4]] != ["onset_s", "offset_s", "pitch", "velocity"]:
            print(f"warning: {path} header is {header!r}, expected onset_s/offset_s/pitch/velocity; "
                  f"reading it as data anyway is not attempted", file=sys.stderr)
        for row in reader:
            if not row:
                continue
            onset_s, offset_s, pitch, velocity = row[:4]
            events.append((float(onset_s), float(offset_s), float(pitch), float(velocity)))
    return events


def _read_var_len(data, pos):
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not (byte & 0x80):
            break
    return value, pos


def _read_track_events(data, pos, track_end):
    """(abs_tick, status, meta_type_or_None, payload_bytes) for one MTrk chunk, decoding running
    status. Meta events carry their type in the third slot; channel events carry None there."""
    events = []
    tick = 0
    running_status = None

    while pos < track_end:
        delta, pos = _read_var_len(data, pos)
        tick += delta

        status = data[pos]
        if status & 0x80:
            pos += 1
            if status < 0xF0:
                running_status = status
        else:
            status = running_status

        if status is None:
            raise ValueError("MIDI file uses running status before any status byte was seen")

        if status == 0xFF:
            meta_type = data[pos]
            pos += 1
            length, pos = _read_var_len(data, pos)
            payload = data[pos:pos + length]
            pos += length
            events.append((tick, 0xFF, meta_type, payload))
        elif status in (0xF0, 0xF7):
            length, pos = _read_var_len(data, pos)
            pos += length  # sysex carries no note information
        else:
            nbytes = 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
            payload = data[pos:pos + nbytes]
            pos += nbytes
            events.append((tick, status, None, payload))

    return events


def load_notes_midi(path):
    """Minimal SMF reader: metrical time division only (matches make_corpus.py's hand-rolled
    writer), formats 0/1/2 all handled by merging every track's note events onto one tempo map.
    Kept dependency-free on purpose, same as the rest of tools/bench."""
    data = path.read_bytes()

    if data[0:4] != b"MThd":
        raise ValueError(f"{path}: not a MIDI file (missing MThd)")

    header_len = struct.unpack(">I", data[4:8])[0]
    _fmt, ntrks, division = struct.unpack(">HHH", data[8:8 + header_len])
    if division & 0x8000:
        raise ValueError(f"{path}: SMPTE time division is not supported")
    ticks_per_quarter = division

    pos = 8 + header_len
    tracks = []
    for _ in range(ntrks):
        if data[pos:pos + 4] != b"MTrk":
            raise ValueError(f"{path}: expected MTrk chunk at offset {pos}")
        length = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        track_start = pos + 8
        track_end = track_start + length
        tracks.append(_read_track_events(data, track_start, track_end))
        pos = track_end

    tempo_changes = {0: 500000}  # default 120 BPM until the first tempo meta event
    for events in tracks:
        for tick, status, meta_type, payload in events:
            if status == 0xFF and meta_type == 0x51:
                tempo_changes[tick] = struct.unpack(">I", b"\x00" + bytes(payload))[0]
    tempo_changes = sorted(tempo_changes.items())

    def tick_to_seconds(target_tick):
        seconds = 0.0
        prev_tick, prev_tempo = tempo_changes[0]
        for change_tick, tempo in tempo_changes[1:]:
            if change_tick >= target_tick:
                break
            seconds += (change_tick - prev_tick) * (prev_tempo / 1e6) / ticks_per_quarter
            prev_tick, prev_tempo = change_tick, tempo
        seconds += (target_tick - prev_tick) * (prev_tempo / 1e6) / ticks_per_quarter
        return seconds

    events_out = []
    for events in tracks:
        pending = {}  # (channel, pitch) -> [(onset_tick, velocity), ...], FIFO
        for tick, status, meta_type, payload in events:
            if status == 0xFF:
                continue
            kind = status & 0xF0
            channel = status & 0x0F
            if kind == 0x90 and payload[1] != 0:  # note on
                pending.setdefault((channel, payload[0]), []).append((tick, payload[1]))
            elif kind == 0x80 or (kind == 0x90 and payload[1] == 0):  # note off
                key = (channel, payload[0])
                queue = pending.get(key)
                if queue:
                    onset_tick, velocity = queue.pop(0)
                    # Normalised to 0..1 to match Notes::Event::velocity's convention. The
                    # weighting formula (duration * velocity) is scale-invariant under Pearson
                    # correlation and the support gate's ratio test either way, so this is for
                    # readability, not correctness -- see the --dump-notes TSV path, which is
                    # deliberately left on its native 0-127 scale and still scores identically.
                    events_out.append((tick_to_seconds(onset_tick), tick_to_seconds(tick),
                                       float(payload[0]), velocity / 127.0))

    return events_out


def find_notes_file(notes_dir, track_id):
    for suffix in (".est.tsv", ".tsv", ".mid", ".midi"):
        candidate = notes_dir / f"{track_id}{suffix}"
        if candidate.exists():
            return candidate
    return None


def load_events(path):
    name = path.name.lower()
    if name.endswith(".est.tsv") or name.endswith(".tsv"):
        return load_notes_tsv(path)
    if path.suffix.lower() in (".mid", ".midi"):
        return load_notes_midi(path)
    raise ValueError(f"unrecognised note-file extension: {path}")


def load_labels(path):
    labels = {}
    with open(path, encoding="utf-8", newline="") as f:
        reader = csv.reader(f, delimiter="\t")
        header = next(reader, None)
        for row in reader:
            if not row:
                continue
            track_id, tonic, mode = row[0], row[1], row[2]
            if tonic not in NOTE_INDEX or mode not in ("major", "minor"):
                print(f"skipping malformed label row: {row}", file=sys.stderr)
                continue
            labels[track_id] = (NOTE_INDEX[tonic], mode)
    return labels


# --- Main scoring run ------------------------------------------------------------------------

def run_scoring(notes_dir, labels_path):
    labels = load_labels(labels_path)
    if not labels:
        print(f"No usable labels in {labels_path}", file=sys.stderr)
        return 1

    results = {kind: {"scored": 0, "correct": 0, "mirex_sum": 0.0, "confusion": Counter()}
               for kind, _ in PROFILE_LABELS}

    total = len(labels)
    missing = 0
    unscorable = 0

    for track_id, (ref_tonic, ref_mode) in sorted(labels.items()):
        path = find_notes_file(notes_dir, track_id)
        if path is None:
            missing += 1
            continue

        events = load_events(path)
        histogram = build_histogram(events)
        if histogram is None:
            unscorable += 1
            continue

        for kind, _ in PROFILE_LABELS:
            est_tonic, est_mode_raw, _corr = score_profile_set(histogram, kind)
            category, score = classify(ref_tonic, ref_mode, est_tonic, est_mode_raw)
            r = results[kind]
            r["scored"] += 1
            if category == "correct":
                r["correct"] += 1
            r["mirex_sum"] += score
            r["confusion"][category] += 1

    print(f"tracks: {total} labelled, {missing} with no note file found, "
          f"{unscorable} unscorable (failed the pitch-class support gate)")
    print()
    print(f"{'profile set':32s} {'accuracy':>9s} {'mirex':>7s} {'n':>5s}   confusion")
    for kind, label in PROFILE_LABELS:
        r = results[kind]
        n = r["scored"]
        accuracy = r["correct"] / n if n else 0.0
        mirex = r["mirex_sum"] / n if n else 0.0
        confusion = ", ".join(f"{k}={v}" for k, v in sorted(r["confusion"].items()) if k != "correct")
        print(f"{label:32s} {accuracy:9.3f} {mirex:7.3f} {n:5d}   {confusion}")

    return 0


# --- Self-test ---------------------------------------------------------------------------------

def synth_histogram(profile, tonic, rng, noise=0.02):
    """An ideal histogram for `tonic` built directly from `profile`'s own shape, plus a little
    deterministic jitter so recovery is tested on near-ideal data, not exact equality."""
    histogram = [0.0] * 12
    for i in range(12):
        histogram[(i + tonic) % 12] = profile[i]
    peak = max(profile)
    for i in range(12):
        histogram[i] = max(0.0, histogram[i] + rng.uniform(-noise, noise) * peak)
    return histogram


def self_test_profile_set(label, kind, major, minor, rng):
    misses = []
    for tonic in range(12):
        for mode, profile in (("major", major), ("minor", minor)):
            histogram = synth_histogram(profile, tonic, rng)
            got_tonic, got_mode, _ = best_kkt_style(histogram, major, minor)
            if (got_tonic, got_mode) != (tonic, mode):
                misses.append((f"{NOTE_NAMES[tonic]} {mode}", f"{NOTE_NAMES[got_tonic]} {got_mode}"))

    status = "OK (24/24)" if not misses else f"FAILED ({len(misses)}/24 missed)"
    print(f"  {label}: {status}")
    for want, got in misses:
        print(f"    wanted {want}, got {got}")
    return not misses


def self_test_modal(rng):
    misses = []
    for tonic in range(12):
        for mode, mode_name in (("major", "Ionian"), ("minor", "Aeolian")):
            histogram = synth_histogram(MODAL_TEMPLATES[mode_name], tonic, rng)
            got_tonic, got_name, _ = best_modal(histogram)
            if (got_tonic, got_name) != (tonic, mode_name):
                want = f"{NOTE_NAMES[tonic]} {mode_name} ({mode})"
                got = f"{NOTE_NAMES[got_tonic]} {got_name}"
                misses.append((want, got))

    status = "OK (24/24)" if not misses else f"FAILED ({len(misses)}/24 missed)"
    print(f"  Modal (7-mode, doubled tonic): {status}")
    for want, got in misses:
        print(f"    wanted {want}, got {got}")
    return not misses


def self_test_structural_limits():
    print()
    print("Structural limits: what a pitch-class histogram cannot decide")
    print("-" * 63)

    # C Ionian and A Aeolian share every pitch class {C,D,E,F,G,A,B}; only which one is "the
    # tonic" differs. Build the C-Ionian histogram once and score it two ways.
    plain_ionian_hist = list(PLAIN_TEMPLATES["Ionian"])  # tonic C sits at position 0, undoubled
    as_ionian = correlate(plain_ionian_hist, PLAIN_TEMPLATES["Ionian"], 0)
    as_aeolian = correlate(plain_ionian_hist, PLAIN_TEMPLATES["Aeolian"], 9)
    same = abs(as_ionian - as_aeolian) < 1e-9
    print(f"  Undoubled binary template: the same C-major-scale histogram scores {as_ionian:.6f} "
          f"read as 'C Ionian' and {as_aeolian:.6f} read as 'A Aeolian' -- "
          f"{'IDENTICAL' if same else 'different'}.")
    print("  That is structural, not a tuning gap: Pearson correlation of a flat 0/1 template")
    print("  depends only on which pitch classes are in scale, never on which one is called the")
    print("  tonic, so relative major/minor -- and every other pair of modes sharing one seven-")
    print("  note collection -- is provably indistinguishable from an unweighted binary template.")

    doubled_ionian_hist = list(MODAL_TEMPLATES["Ionian"])
    d_ionian = correlate(doubled_ionian_hist, MODAL_TEMPLATES["Ionian"], 0)
    d_aeolian = correlate(doubled_ionian_hist, MODAL_TEMPLATES["Aeolian"], 9)
    differ = abs(d_ionian - d_aeolian) > 1e-9
    print(f"\n  Doubled-tonic template (this bench's modal set): the same comparison scores "
          f"{d_ionian:.6f} as 'C Ionian' and {d_aeolian:.6f} as 'A Aeolian' -- "
          f"{'now different' if differ else 'STILL IDENTICAL'}.")
    print("  Doubling the tonic breaks the tie in principle, but on a signal of one bin out of")
    print("  twelve, so relative-key confusion is expected to remain the modal set's weakest")
    print("  point on real (noisy) material even though the recovery test above uses clean")
    print("  synthetic data and passes.")
    print()
    print("  Krumhansl-Kessler, Temperley-Kostka-Payne and Albrecht-Shanahan do not have this")
    print("  problem in the first place: their major and minor vectors are two different smooth")
    print("  shapes, not a shared 0/1 set with one bin doubled, so relative keys pull apart across")
    print("  every bin rather than just the tonic's.")


def run_self_test(seed):
    rng = random.Random(seed)
    print("Self-test: synthesizing an ideal histogram from each profile's own template for all")
    print("24 major/minor keys, and checking the profile set recovers its own key.\n")

    ok = True
    ok &= self_test_profile_set("Krumhansl-Kessler", "kk", KK_MAJOR, KK_MINOR, rng)
    ok &= self_test_profile_set("Temperley-Kostka-Payne", "tkp", TKP_MAJOR, TKP_MINOR, rng)
    ok &= self_test_profile_set("Albrecht-Shanahan", "as", AS_MAJOR, AS_MINOR, rng)
    ok &= self_test_modal(rng)

    self_test_structural_limits()

    print()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("notes_dir", nargs="?", type=pathlib.Path,
                        help="directory of <track_id>.est.tsv / .tsv / .mid note files")
    parser.add_argument("labels_tsv", nargs="?", type=pathlib.Path,
                        help="labels TSV: track_id, tonic, mode (see fetch_giantsteps.py)")
    parser.add_argument("--self-test", action="store_true",
                        help="synthesize ideal histograms and verify each profile set recovers "
                             "its own keys, then exit")
    parser.add_argument("--seed", type=int, default=42, help="RNG seed for the self-test's noise")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test(args.seed)

    if not args.notes_dir or not args.labels_tsv:
        parser.error("notes_dir and labels_tsv are required unless --self-test is given")
    if not args.notes_dir.is_dir():
        parser.error(f"not a directory: {args.notes_dir}")
    if not args.labels_tsv.is_file():
        parser.error(f"not a file: {args.labels_tsv}")

    return run_scoring(args.notes_dir, args.labels_tsv)


if __name__ == "__main__":
    sys.exit(main())
