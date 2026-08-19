#!/usr/bin/env python3
"""Lay out the GiantSteps audio cache as a bench corpus, so Bench.exe can dump its notes.

Bench.exe pairs each audio file with a sibling <name>.mid reference and skips anything without
one, but the GiantSteps Key dataset has no note-level ground truth at all -- only key labels.
The key bench never needs the bench's note scores; it needs the --dump-notes side effect, which
Bench writes before it scores anything. So this script builds Tests/bench_corpus_giantsteps/ by
hardlinking (or copying, on link failure) each verified mp3 out of
%USERPROFILE%/.okstudio/giantsteps-cache/audio/ and putting a one-note stub .mid beside it,
purely to get past the pairing check. Every note-level column the bench prints for this corpus
is meaningless by construction; the .est.tsv files in the --dump-notes directory are the output.

Only tracks present in the labels TSV (tools/keybench/giantsteps_labels.tsv, written by
fetch_giantsteps.py) are laid out, so a stray file in the cache never grows the corpus, and the
count printed at the end is directly comparable to the labelled total (604).

The stub is a minimal format-0 SMF: one C4 at t=0 for one 480-tick beat. A note is included
rather than an empty track because an empty reference exercises the scorer's zero-denominator
edges for no benefit; one note keeps every score finite and obviously junk.

Usage:
  py tools/keybench/make_giantsteps_corpus.py
  py tools/keybench/make_giantsteps_corpus.py --out Tests/bench_corpus_giantsteps

Then:
  build\\tools\\bench\\Bench_artefacts\\Release\\Bench.exe Tests\\bench_corpus_giantsteps
      --dump-notes tools\\keybench\\out\\dump_cpu
  py tools/keybench/score_keys.py tools/keybench/out/dump_cpu tools/keybench/giantsteps_labels.tsv
"""

import argparse
import csv
import os
import pathlib
import shutil
import sys

CACHE_AUDIO = (
    pathlib.Path(os.environ.get("USERPROFILE", os.path.expanduser("~")))
    / ".okstudio"
    / "giantsteps-cache"
    / "audio"
)

LABELS_TSV = pathlib.Path(__file__).resolve().parent / "giantsteps_labels.tsv"

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO_ROOT / "Tests" / "bench_corpus_giantsteps"

# Format 0, division 480: note-on C4 vel 64 at delta 0, note-off at delta 480, end of track.
STUB_MID = bytes.fromhex("4d546864000000060000000101e0" "4d54726b0000000d" "00903c40" "8360803c40" "00ff2f00")


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="corpus directory to create")
    args = parser.parse_args()

    if not LABELS_TSV.exists():
        print(f"labels file missing: {LABELS_TSV} (run fetch_giantsteps.py first)", file=sys.stderr)
        return 1

    with open(LABELS_TSV, newline="", encoding="utf-8") as f:
        track_ids = [row["track_id"] for row in csv.DictReader(f, delimiter="\t")]

    out_dir = pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    laid_out = 0
    missing = 0

    for track_id in track_ids:
        src = CACHE_AUDIO / f"{track_id}.mp3"

        if not src.exists():
            missing += 1
            continue

        dst = out_dir / f"{track_id}.mp3"

        if not dst.exists():
            try:
                os.link(src, dst)
            except OSError:
                shutil.copyfile(src, dst)

        stub = out_dir / f"{track_id}.mid"

        if not stub.exists():
            stub.write_bytes(STUB_MID)

        laid_out += 1

    print(f"{laid_out} of {len(track_ids)} labelled tracks laid out in {out_dir}", end="")
    print(f" ({missing} not in cache; fetch_giantsteps.py --audio-all fills them)" if missing else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
