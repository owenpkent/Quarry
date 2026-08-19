#!/usr/bin/env python3
"""Cross-engine agreement over unlabelled material, scored with the bake-off's own matcher.

Real takes have no ground truth, so per-engine accuracy is unmeasurable on them; what is
measurable is whether independently trained engines converge on the same notes. This scores
every engine pair's outputs against each other with the same `mir_eval` onset matching the
bake-off uses (one engine as reference, the other as estimate, folded to F1 so the direction
does not matter) and writes one row per take. High pairwise F1 means the take is easy and the
transcriptions are trustworthy; all pairs low means the material broke everyone, or sits
outside every engine's idea of music. Engines with different instrument scopes (a generalist
versus a piano specialist on a full mix) disagree by scope rather than accuracy, so read the
note counts next to the scores before calling a take broken.

Input layout: <outputs-dir>/<engine>/<take>.mid, as run_bakeoff.py writes it. Takes are the
.mid stems present for every engine; an engine missing a take drops that take with a warning
rather than failing the sweep. Output: <outputs-dir>/agreement.tsv (take, optional duration,
per-engine note counts, one F1 column per engine pair) plus a printed summary of per-pair
means and, when --trust-pair engines are present, take counts bucketed by that pair's
agreement (>=0.8 solid, 0.4-0.8 mixed, <0.4 broken).

Usage:
  py tools/bakeoff/agreement.py tools/bakeoff/out/recorded_all --audio-dir <dir-of-wavs>
  py tools/bakeoff/agreement.py <outputs-dir> --engines quarry,kong,transkun,muscriptor
"""

import argparse
import itertools
import pathlib
import sys

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from mir_eval_scoring import load_notes_from_midi, score_case


def f1(counts) -> float:
    p = counts.matched / counts.estimated if counts.estimated else 0.0
    r = counts.matched / counts.reference if counts.reference else 0.0
    return 2 * p * r / (p + r) if (p + r) else 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("outputs_dir", type=pathlib.Path, help="directory of <engine>/<take>.mid")
    parser.add_argument("--engines", default=None, help="comma separated; default: every subdirectory")
    parser.add_argument("--audio-dir", type=pathlib.Path, default=None,
                        help="directory of <take>.wav, for a duration column (needs soundfile)")
    parser.add_argument("--trust-pair", default="transkun,muscriptor",
                        help="engine pair whose mutual agreement buckets the summary")
    args = parser.parse_args()

    if args.engines:
        engines = [e.strip() for e in args.engines.split(",") if e.strip()]
    else:
        engines = sorted(d.name for d in args.outputs_dir.iterdir() if d.is_dir() and any(d.glob("*.mid")))

    if len(engines) < 2:
        print(f"need at least two engine directories under {args.outputs_dir}", file=sys.stderr)
        return 1

    pairs = list(itertools.combinations(engines, 2))
    per_engine = {e: {p.stem for p in (args.outputs_dir / e).glob("*.mid")} for e in engines}
    takes = sorted(set.intersection(*per_engine.values()))
    every_take = set.union(*per_engine.values())

    for engine, stems in per_engine.items():
        if stems != every_take:
            print(f"note: {engine} is missing {len(every_take - stems)} take(s); those takes are dropped",
                  file=sys.stderr)

    if not takes:
        print("no takes present for every engine", file=sys.stderr)
        return 1

    rows = []

    for take in takes:
        notes = {e: load_notes_from_midi(args.outputs_dir / e / f"{take}.mid") for e in engines}
        row = {"take": take}

        if args.audio_dir is not None:
            import soundfile as sf

            wav = args.audio_dir / f"{take}.wav"
            row["dur_s"] = round(sf.info(str(wav)).duration, 1) if wav.exists() else ""

        for e in engines:
            row[f"n_{e}"] = notes[e][1].shape[0]

        for a, b in pairs:
            onset, _ = score_case(*notes[a], *notes[b])
            row[f"{a[:4]}_{b[:4]}"] = round(f1(onset), 3)

        rows.append(row)

    out_tsv = args.outputs_dir / "agreement.tsv"
    cols = list(rows[0].keys())

    with open(out_tsv, "w", encoding="utf-8") as handle:
        handle.write("\t".join(cols) + "\n")

        for row in rows:
            handle.write("\t".join(str(row[c]) for c in cols) + "\n")

    print(f"{len(takes)} takes x {len(engines)} engines -> {out_tsv}\n")

    for a, b in pairs:
        vals = [r[f"{a[:4]}_{b[:4]}"] for r in rows]
        print(f"mean {a}/{b}: {sum(vals) / len(vals):.3f}")

    trust = [e.strip() for e in args.trust_pair.split(",")]

    if len(trust) == 2 and all(e in engines for e in trust):
        key = f"{trust[0][:4]}_{trust[1][:4]}"

        if key not in rows[0]:
            key = f"{trust[1][:4]}_{trust[0][:4]}"

        buckets = [("solid  (>=0.8)", lambda v: v >= 0.8),
                   ("mixed  (0.4-0.8)", lambda v: 0.4 <= v < 0.8),
                   ("broken (<0.4)", lambda v: v < 0.4)]
        print(f"\nby {trust[0]}/{trust[1]} agreement:")

        for label, test in buckets:
            group = [r for r in rows if test(r[key])]
            print(f"  {label}: {len(group)} takes")

    return 0


if __name__ == "__main__":
    sys.exit(main())
