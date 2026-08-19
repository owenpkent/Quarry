#!/usr/bin/env python3
"""Cross-check Bench.cpp's own note-level scorer against mir_eval, the reference implementation
used across the wider transcription literature. If the two disagree by more than rounding, one of
them has a bug worth finding before either number is trusted for anything.

Usage: crosscheck_mir_eval.py <corpus-dir> <dump-dir>

<corpus-dir> holds <name>.wav/<name>.mid pairs, e.g. Tests/bench_corpus.
<dump-dir> holds the <name>.est.tsv files Bench.exe writes with --dump-notes: header
onset_s/offset_s/pitch/velocity, one transcribed note per line, pitch as a MIDI note number.

Prints precision/recall/F1 per case and aggregate, onset-only at 50 ms tolerance and onset+offset
at max(50 ms, 20% of the reference note's duration), matching what Bench.cpp itself reports so the
two can be compared column for column.
"""

import argparse
import pathlib
import sys

from mir_eval_scoring import Counts, load_notes_from_midi, load_notes_from_tsv, print_table, score_case


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("corpus_dir", type=pathlib.Path, help="directory of <name>.wav/<name>.mid pairs")
    parser.add_argument("dump_dir", type=pathlib.Path, help="directory of <name>.est.tsv files")
    args = parser.parse_args()

    if not args.corpus_dir.is_dir():
        print(f"Not a directory: {args.corpus_dir}", file=sys.stderr)
        return 1

    if not args.dump_dir.is_dir():
        print(f"Not a directory: {args.dump_dir}", file=sys.stderr)
        return 1

    suffix = ".est.tsv"
    names = sorted(p.name[: -len(suffix)] for p in args.dump_dir.glob(f"*{suffix}"))

    rows = []
    onset_total = Counts(0, 0, 0)
    onset_offset_total = Counts(0, 0, 0)

    for name in names:
        reference_file = args.corpus_dir / f"{name}.mid"
        estimate_file = args.dump_dir / f"{name}{suffix}"

        if not reference_file.exists():
            print(f"  skipping {name}: no reference at {reference_file}", file=sys.stderr)
            continue

        ref_intervals, ref_pitches = load_notes_from_midi(reference_file)
        est_intervals, est_pitches = load_notes_from_tsv(estimate_file)

        onset, onset_offset = score_case(ref_intervals, ref_pitches, est_intervals, est_pitches)
        rows.append((name, onset, onset_offset))
        onset_total = onset_total + onset
        onset_offset_total = onset_offset_total + onset_offset

    if not rows:
        print(f"No usable pairs between {args.corpus_dir} and {args.dump_dir}", file=sys.stderr)
        return 1

    print_table(rows, onset_total, onset_offset_total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
