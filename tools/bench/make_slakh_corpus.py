#!/usr/bin/env python3
"""Build the "BabySlakh" bench corpus: real multitrack songs (guitar, bass, piano, organ,
strings, ...) synthesized from MIDI, mixed down to one audio file, with exact per-instrument
ground truth available separately. This exercises the transcription engine on dense, harmonically
rich, multi-timbral material, unlike make_real_corpus.py's solo piano or make_corpus.py's synthetic
single-voice cases: same <name>.wav / <name>.mid contract, but the "instrument" being transcribed
is really a small band.

Requires the local BabySlakh cache at ~/.okstudio/slakh-cache/babyslakh_16k/, downloaded and
unpacked from Zenodo record 4603870 (babyslakh_16k.tar.gz, CC-BY licensed, 20 tracks: the first
20 tracks of the full Slakh2100 dataset). There is no fetch script for this one (a single ~880 MB
tar.gz, hand-downloaded); see the record at https://zenodo.org/records/4603870 if the cache needs
rebuilding.

Cache layout actually found on disk under a TrackNNNNN/ directory (confirmed against real files,
not assumed): mix.wav (the rendered mixdown, mono 16-bit PCM @ 16kHz for every track surveyed),
metadata.yaml (a `stems:` dict keyed by stem id like "S00", each with at least `inst_class`,
`is_drum`, `midi_program_name`, `program_num`; some listed stems, e.g. unrendered Sound Effects
stems, have no matching file on disk and must be skipped), MIDI/S<NN>.mid and stems/S<NN>.wav per
rendered stem. The per-stem audio (stems/S<NN>.wav) is not used by this script, but it exists in
the same cache for a future source-separation evaluation.

What this does:

  1. Lists the 20 track directories, sorted by name, and deterministically samples up to 12 of them
     (one random.Random(SEED) draw) to keep the corpus a manageable size while still covering most
     of BabySlakh.
  2. For each chosen track, reads metadata.yaml and merges the note-onset times of every stem whose
     `is_drum` is false (i.e. every pitched instrument: guitars, bass, keys, strings, horns, etc,
     but not the drum kit) that actually has a MIDI file on disk.
  3. Deterministically (same rng, sorted track order) picks a 30-second excerpt window per track
     with a healthy number of onsets in it (see MIN_ONSETS below for why the bar is set well above
     make_real_corpus.py's solo-piano threshold).
  4. Ground truth <name>.mid: for every pitched stem, notes whose onset falls inside the window,
     original pitch/velocity, offsets clipped to the window end, all merged into a single output
     instrument track (there is no one "correct" program for a merged multi-instrument transcript,
     so see GT_PROGRAM below). No pedal handling: unlike the MAESTRO Disklaviers, these are
     sequenced MIDI stems, not real sustain-pedal performances.
  5. Audio <name>.wav: mix.wav sliced to the same window, written out as-is at its native sample
     rate (16kHz here) and bit depth (16-bit PCM) -- no resampling to 44100, Bench reads audio at
     whatever rate the wav declares.

Usage:  tools/bench/.venv/Scripts/python.exe tools/bench/make_slakh_corpus.py [output-dir]
"""

import argparse
import array
import bisect
import os
import pathlib
import random
import sys
import wave

import pretty_midi
import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE_DIR = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / ".okstudio" / "slakh-cache"
SLAKH_ROOT = CACHE_DIR / "babyslakh_16k"
DEFAULT_OUTPUT = REPO_ROOT / "Tests" / "bench_corpus_slakh"

# Fixed so the corpus is exactly reproducible: same 12 tracks, same 12 windows, every time, as
# long as the babyslakh_16k cache is the one Zenodo record 4603870 unpacks to.
SEED = 20260818

WINDOW_SECONDS = 30.0
MAX_TRACKS = 12

# make_real_corpus.py targets >= 20 onsets per 30s window on solo piano performances. A BabySlakh
# window is the union of every pitched stem in a track (guitar + bass + piano + organ + strings,
# etc, all merged), so it is far denser: a survey of every track's busiest 30s window (see the
# script's own dev notes, not shipped here) found a low of ~340 onsets and a high of ~1140. 100 is
# comfortably above MAESTRO's floor (so windows are never degenerate/near-silent) and comfortably
# below every track's own ceiling (so the fast random-offset search always wins and the full grid
# scan fallback is never actually needed), rather than a floor chosen to force maximum density.
MIN_ONSETS = 100

# This ground truth merges notes from several instruments (guitar, bass, organ, strings, ...) into
# one instrument track, so there is no single "correct" program number for it; 0 (Acoustic Grand
# Piano) is just a reasonable, inert default, same choice make_real_corpus.py makes for its own
# (actually-piano) ground truth.
GT_PROGRAM = 0


def load_metadata(track_dir):
    with open(track_dir / "metadata.yaml", encoding="utf-8") as f:
        return yaml.safe_load(f)


def pitched_stem_ids(meta, track_dir):
    """Stem ids from metadata.yaml with is_drum == false AND an actual MIDI file on disk (some
    listed stems, e.g. unrendered Sound Effects stems, have metadata but no rendered file).
    """
    ids = []
    for sid, info in sorted(meta.get("stems", {}).items()):
        if info.get("is_drum"):
            continue
        if not (track_dir / "MIDI" / f"{sid}.mid").exists():
            continue
        ids.append(sid)
    return ids


def load_track_notes(track_dir, stem_ids):
    """All notes from all pitched stems, each tagged with which stem it came from (for nothing
    more than debugging/print purposes -- the ground truth merges them regardless of source).
    """
    notes = []
    for sid in stem_ids:
        pm = pretty_midi.PrettyMIDI(str(track_dir / "MIDI" / f"{sid}.mid"))
        for inst in pm.instruments:
            notes.extend(inst.notes)
    notes.sort(key=lambda n: n.start)
    return notes


def wav_duration(path):
    with wave.open(str(path), "rb") as w:
        return w.getnframes() / w.getframerate()


# --- excerpt selection + ground truth --------------------------------------------------------


def pick_excerpt_start(notes_start_times, duration, rng):
    """Deterministically (given rng's state) find a WINDOW_SECONDS start with >= MIN_ONSETS
    onsets inside it. Tries random offsets first (so the corpus isn't just "the first 30s of
    every track"), then falls back to a full grid scan so a qualifying window is always found
    if one exists. Mirrors make_real_corpus.py's pick_excerpt_start exactly.
    """
    margin = 5.0
    lo, hi = margin, duration - WINDOW_SECONDS - margin
    if hi <= lo:
        lo, hi = 0.0, max(0.0, duration - WINDOW_SECONDS)

    def onset_count(start):
        end = start + WINDOW_SECONDS
        lo_i = bisect.bisect_left(notes_start_times, start)
        hi_i = bisect.bisect_left(notes_start_times, end)
        return hi_i - lo_i

    for _ in range(200):
        start = rng.uniform(lo, hi) if hi > lo else lo
        if onset_count(start) >= MIN_ONSETS:
            return start

    step = 1.0
    start = 0.0
    while start + WINDOW_SECONDS <= duration:
        if onset_count(start) >= MIN_ONSETS:
            return start
        start += step
    return None


def build_ground_truth(notes, start, end):
    gt_notes = []
    for n in notes:
        if not (start <= n.start < end):
            continue
        onset = n.start - start
        offset = min(n.end, end) - start
        offset = max(offset, onset + 0.005)
        gt_notes.append((onset, offset, n.pitch, n.velocity))
    gt_notes.sort(key=lambda t: (t[0], t[2]))
    return gt_notes


def write_ground_truth_midi(path, gt_notes):
    pm = pretty_midi.PrettyMIDI(resolution=10000, initial_tempo=120.0)
    inst = pretty_midi.Instrument(program=GT_PROGRAM, name="slakh-mix")
    for onset, offset, pitch, velocity in gt_notes:
        inst.notes.append(pretty_midi.Note(velocity=int(velocity), pitch=int(pitch), start=onset, end=offset))
    pm.instruments.append(inst)
    pm.write(str(path))


# --- audio slicing ---------------------------------------------------------------------------


def slice_wav(mix_path, start, end, out_path):
    with wave.open(str(mix_path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        framerate = w.getframerate()
        assert sampwidth == 2, f"expected 16-bit PCM, got {sampwidth * 8}-bit ({mix_path})"
        frames = w.readframes(w.getnframes())

    samples = array.array("h")
    samples.frombytes(frames)

    start_frame = round(start * framerate)
    window_frames = round((end - start) * framerate)
    start_idx = start_frame * channels
    want = window_frames * channels

    window = samples[start_idx : start_idx + want]
    if len(window) < want:
        window = window + array.array("h", [0]) * (want - len(window))

    with wave.open(str(out_path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(framerate)
        w.writeframes(window.tobytes())


# --- driving it all ------------------------------------------------------------------------


def process_case(name, track_dir, rng, output_dir):
    meta = load_metadata(track_dir)
    stem_ids = pitched_stem_ids(meta, track_dir)
    if not stem_ids:
        print(f"  skipping {name}: no pitched stems with a MIDI file in {track_dir.name}")
        return None

    notes = load_track_notes(track_dir, stem_ids)
    duration = wav_duration(track_dir / "mix.wav")

    note_starts = [n.start for n in notes]
    start = pick_excerpt_start(note_starts, duration, rng)
    if start is None:
        print(f"  skipping {name}: no {WINDOW_SECONDS:.0f}s window with >= {MIN_ONSETS} onsets in {track_dir.name}")
        return None
    end = start + WINDOW_SECONDS

    gt_notes = build_ground_truth(notes, start, end)

    slice_wav(track_dir / "mix.wav", start, end, output_dir / f"{name}.wav")
    write_ground_truth_midi(output_dir / f"{name}.mid", gt_notes)

    return {
        "name": name,
        "notes": len(gt_notes),
        "stems": len(stem_ids),
        "start": start,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", nargs="?", type=pathlib.Path, default=DEFAULT_OUTPUT)
    arguments = parser.parse_args()

    if not SLAKH_ROOT.exists():
        raise SystemExit(
            f"{SLAKH_ROOT} not found; download+unpack babyslakh_16k.tar.gz from "
            f"https://zenodo.org/records/4603870 into {CACHE_DIR} first"
        )

    arguments.output.mkdir(parents=True, exist_ok=True)
    print(f"slakh root: {SLAKH_ROOT}")
    print(f"output:     {arguments.output}\n")

    track_dirs = sorted(p for p in SLAKH_ROOT.iterdir() if p.is_dir() and p.name.startswith("Track"))
    print(f"found {len(track_dirs)} tracks")

    rng = random.Random(SEED)
    # One fixed-seed sample of up to MAX_TRACKS out of the sorted track list, re-sorted afterwards
    # so processing order (and so every subsequent rng draw in pick_excerpt_start) is deterministic
    # regardless of what rng.sample's internal shuffling did.
    chosen_dirs = rng.sample(track_dirs, min(MAX_TRACKS, len(track_dirs)))
    chosen_dirs.sort(key=lambda p: p.name)
    print(f"selected {len(chosen_dirs)} tracks: {', '.join(p.name for p in chosen_dirs)}\n")

    results = []
    for idx, track_dir in enumerate(chosen_dirs, start=1):
        name = f"slakh_{track_dir.name.lower()}"
        print(f"[{idx}/{len(chosen_dirs)}] {name}  <- {track_dir.name}")
        result = process_case(name, track_dir, rng, arguments.output)
        if result:
            results.append(result)
            print(f"    {result['notes']} gt notes from {result['stems']} pitched stems, window start {result['start']:.1f}s")

    print(f"\nwrote {len(results)} pairs to {arguments.output}")
    if results:
        note_counts = sorted(r["notes"] for r in results)
        mid = note_counts[len(note_counts) // 2]
        print(f"  notes per case: min {note_counts[0]}, median {mid}, max {note_counts[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
