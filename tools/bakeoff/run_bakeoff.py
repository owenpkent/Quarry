#!/usr/bin/env python3
"""Run one or more transcription engines over a corpus and score every one of them with the same
mir_eval code, so a candidate engine's claims about itself land on the same yardstick as Quarry's
own bench.

Usage: run_bakeoff.py <corpus-dir> --engines kong,transkun[,dump,muscriptor]
                       [--dump-dir DIR] [--cases name1,name2,...] [--out-dir DIR]

Engines:
  dump       Re-emits Bench.exe's pre-existing <name>.est.tsv files (see --dump-dir, produced by
             `Bench --dump-notes`) as MIDI, so Quarry's own engine goes through the identical
             scoring path as everyone else here rather than a hand-trusted shortcut.
  kong       Kong et al.'s piano_transcription_inference. Downloads its checkpoint on first use.
  transkun   Downloads its checkpoint on first use.
  muscriptor Only runs if the package is installed; skipped with a message otherwise.

Each engine's output lands at <out-dir>/<engine>/<name>.mid (default out-dir:
tools/bakeoff/out, next to this script). Model checkpoints download on first use; that is
expected, not an error.
"""

import argparse
import pathlib
import subprocess
import sys
import time

from mir_eval_scoring import Counts, load_notes_from_midi, print_table, score_case, write_notes_midi

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent


def find_cases(corpus_dir: pathlib.Path, only=None):
    names = sorted(p.stem for p in corpus_dir.glob("*.wav") if (corpus_dir / f"{p.stem}.mid").exists())

    if only is not None:
        names = [n for n in names if n in only]

    return names


def run_dump(name, dump_dir):
    """Re-read a case's estimate from Bench.exe's --dump-notes TSV as a list of
    (onset, offset, pitch, velocity) tuples, ready for write_notes_midi."""
    tsv_path = pathlib.Path(dump_dir) / f"{name}.est.tsv"

    if not tsv_path.exists():
        raise FileNotFoundError(f"no dump for {name!r} at {tsv_path}")

    notes = []

    with open(tsv_path, "r", encoding="utf-8") as handle:
        next(handle, None)  # header line

        for raw in handle:
            line = raw.strip()

            if not line:
                continue

            onset_s, offset_s, pitch, velocity = line.split("\t")
            notes.append((float(onset_s), float(offset_s), float(pitch), float(velocity)))

    return notes


def make_kong_transcriber():
    from piano_transcription_inference import PianoTranscription

    return PianoTranscription(device="cpu")


def run_kong(transcriber, wav_path, out_path):
    """Kong et al.'s piano_transcription_inference writes MIDI directly; nothing to convert."""
    from piano_transcription_inference import load_audio
    from piano_transcription_inference import sample_rate as kong_sample_rate

    audio, _ = load_audio(str(wav_path), sr=kong_sample_rate, mono=True)
    transcriber.transcribe(audio, str(out_path))


def make_transkun_transcriber():
    # transkun's own entry point (transkun.transcribe:main) parses sys.argv directly rather than
    # exposing a Python API, so it is driven the same way its own console script would be: as a
    # subprocess. Its pretrained weight ships inside the package (transkun/pretrained/2.0.*), so
    # unlike kong there is no checkpoint download to wait on.
    return None


def run_transkun(_transcriber, wav_path, out_path):
    subprocess.run([sys.executable, "-m", "transkun.transcribe", str(wav_path), str(out_path), "--device", "cpu"],
                   check=True)


def make_muscriptor_transcriber():
    import torch
    from muscriptor import TranscriptionModel

    # Pulls muscriptor's default checkpoint (gated: needs the licence accepted on the
    # HuggingFace model page and a logged-in token).
    device = "cuda" if torch.cuda.is_available() else "cpu"
    return TranscriptionModel.load_model(device=device)


def run_muscriptor(transcriber, wav_path, out_path):
    # detect_tempo=False keeps onsets in wall-clock time. The default beat-grid path
    # shifts the whole transcription onto a bar grid (BeatGrid.onset_delay), which is
    # right for a DAW drop and was measured here as a constant +1.0 s against ground
    # truth, collapsing F1 to 0.1 for timing reasons alone.
    midi_bytes = transcriber.transcribe_to_midi(str(wav_path), detect_tempo=False)
    pathlib.Path(out_path).write_bytes(midi_bytes)


ENGINES = {
    "kong": (make_kong_transcriber, run_kong),
    "transkun": (make_transkun_transcriber, run_transkun),
    "muscriptor": (make_muscriptor_transcriber, run_muscriptor),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("corpus_dir", type=pathlib.Path)
    parser.add_argument("--engines", required=True, help="comma separated: kong,transkun,dump,muscriptor")
    parser.add_argument("--dump-dir", type=pathlib.Path, default=None, help="required if 'dump' is in --engines")
    parser.add_argument("--cases", default=None, help="comma separated case names; default is every case")
    parser.add_argument("--out-dir", type=pathlib.Path, default=SCRIPT_DIR / "out")
    args = parser.parse_args()

    if not args.corpus_dir.is_dir():
        print(f"Not a directory: {args.corpus_dir}", file=sys.stderr)
        return 1

    engines = [e.strip() for e in args.engines.split(",") if e.strip()]
    only = set(args.cases.split(",")) if args.cases else None
    names = find_cases(args.corpus_dir, only)

    if not names:
        print(f"No <name>.wav/<name>.mid pairs in {args.corpus_dir}", file=sys.stderr)
        return 1

    exit_code = 0

    for engine in engines:
        engine_out_dir = args.out_dir / engine
        engine_out_dir.mkdir(parents=True, exist_ok=True)

        if engine == "muscriptor":
            try:
                import muscriptor  # noqa: F401
            except ImportError:
                print(f"skipping {engine}: not installed", file=sys.stderr)
                continue

        transcriber = None

        if engine != "dump":
            make, _run = ENGINES[engine]
            transcriber = make()

        rows = []
        failed = []
        onset_total = Counts(0, 0, 0)
        onset_offset_total = Counts(0, 0, 0)

        for name in names:
            wav_path = args.corpus_dir / f"{name}.wav"
            mid_path = engine_out_dir / f"{name}.mid"
            started = time.time()

            # A failure on one case is recorded, not raised, so the sweep reaches the end
            # (same policy as fetch_giantsteps.py): a 90-case corpus should never lose its
            # remaining cases to one bad file. KeyboardInterrupt still stops the run.
            try:
                if engine == "dump":
                    if args.dump_dir is None:
                        print("--dump-dir is required for the 'dump' engine", file=sys.stderr)
                        return 1

                    notes = run_dump(name, args.dump_dir)
                    write_notes_midi(mid_path, notes)
                else:
                    _make, run = ENGINES[engine]
                    run(transcriber, wav_path, mid_path)
            except KeyboardInterrupt:
                raise
            except Exception as error:
                failed.append(name)
                exit_code = 1
                print(f"  {engine}/{name}: FAILED ({type(error).__name__}: {error})", file=sys.stderr)
                continue

            elapsed = time.time() - started

            ref_intervals, ref_pitches = load_notes_from_midi(args.corpus_dir / f"{name}.mid")
            est_intervals, est_pitches = load_notes_from_midi(mid_path)

            onset, onset_offset = score_case(ref_intervals, ref_pitches, est_intervals, est_pitches)
            rows.append((name, onset, onset_offset))
            onset_total = onset_total + onset
            onset_offset_total = onset_offset_total + onset_offset

            print(f"  {engine}/{name}: {est_intervals.shape[0]} notes in {elapsed:.1f}s", file=sys.stderr)

        print(f"\n=== {engine} ===")
        print_table(rows, onset_total, onset_offset_total)

        if failed:
            print(f"{engine}: {len(failed)} case(s) failed: {', '.join(failed)}", file=sys.stderr)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
