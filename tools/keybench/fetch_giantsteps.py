#!/usr/bin/env python3
"""Fetch the GiantSteps Key dataset's key annotations for the key-detection bench.

Clones (or updates) a shallow checkout of GiantSteps/giantsteps-key-dataset from GitHub into
%USERPROFILE%/.okstudio/giantsteps-cache/repo, parses every annotations/key/*.key file, and
writes tools/keybench/giantsteps_labels.tsv with columns track_id, tonic, mode.

Repo layout (found by inspection, github.com/GiantSteps/giantsteps-key-dataset):
  annotations/key/<id>.LOFI.key        final key label, one line: "<tonic> major|minor"
  annotations/giantsteps/<id>.LOFI.key same label in the GiantSteps project's own tagged format
  annotations/genre/                   genre labels, unused here
  md5/<id>.LOFI.md5                    md5 of the corresponding audio file, one per track
  audio_dl.sh                          the dataset's own audio downloader (bash + curl)
  README                               documents 604 two-minute Beatport previews, mp3 format

track_id is the annotation filename's stem, e.g. "1004923.LOFI" (from "1004923.LOFI.key"), which
is also the audio filename minus ".mp3" and the natural join key for a note dump produced from
that same audio file later.

Tonic accidentals in the dataset are spelled as flats (Db, Eb, Gb, Ab, Bb); they are normalised to
sharps here (C#, D#, F#, G#, A#) to match Lib/Model/KeyEstimate.cpp's kNoteNames, which the bench
compares estimates against.

Audio: audio_dl.sh's primary URL (https://www.cp.jku.at/datasets/giantsteps/backup/<name>.mp3)
serves the previews (verified: HTTP 200, audio/mpeg, correct Content-Length for a ~2 min mp3).
Its documented backup mirror, geo-samples.beatport.com, 404s for every track as of 2026-08 --
Beatport retired direct file access years ago, and the "backup" is now the only one that works.
--audio-sample proves the mechanism by downloading a handful of files and checking them against
the repo's own md5 hashes; it never exceeds 10 regardless of what is asked for, since a full pull
of ~850 MB is a separate, deliberate step for later.

--audio-all downloads every track (all 604) into %USERPROFILE%/.okstudio/giantsteps-cache/audio/,
sequentially, with a short pause between requests to stay polite to the JKU server. Each file is
verified against the repo's own md5; a failure is retried once, and if still failing is recorded
(not raised) so the run continues to the end. Already-present, already-verified files are skipped
on a re-run, so --audio-all is safe to re-invoke to pick up stragglers.

Usage:
  py tools/keybench/fetch_giantsteps.py
  py tools/keybench/fetch_giantsteps.py --audio-sample 5
  py tools/keybench/fetch_giantsteps.py --audio-all
  py tools/keybench/fetch_giantsteps.py --no-update --out other_labels.tsv
"""

import argparse
import hashlib
import os
import pathlib
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO_URL = "https://github.com/GiantSteps/giantsteps-key-dataset.git"

CACHE_DIR = pathlib.Path(os.environ.get("USERPROFILE", os.path.expanduser("~"))) / ".okstudio" / "giantsteps-cache"
REPO_DIR = CACHE_DIR / "repo"

OUT_TSV = pathlib.Path(__file__).resolve().parent / "giantsteps_labels.tsv"

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
VALID_TONICS = set(NOTE_NAMES)

# The dataset spells accidentals as flats; KeyEstimate.cpp's kNoteNames only knows sharps.
FLAT_TO_SHARP = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#"}

# audio_dl.sh's own two URLs, tried in the order it tries them.
AUDIO_URL_PRIMARY = "https://www.cp.jku.at/datasets/giantsteps/backup/{name}.mp3"
AUDIO_URL_BACKUP = "https://geo-samples.beatport.com/lofi/{name}.mp3"

MAX_AUDIO_SAMPLE = 10


def run_git(args):
    try:
        result = subprocess.run(["git"] + args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    except FileNotFoundError:
        print("git not found on PATH", file=sys.stderr)
        return None
    if result.returncode != 0:
        print(result.stderr.strip(), file=sys.stderr)
        return None
    return result.stdout


def clone_or_update(repo_dir, update):
    repo_dir.parent.mkdir(parents=True, exist_ok=True)

    if (repo_dir / ".git").exists():
        if not update:
            print(f"using existing clone at {repo_dir} (--no-update)")
            return repo_dir
        print(f"updating existing clone at {repo_dir}")
        if run_git(["-C", str(repo_dir), "fetch", "--depth", "1", "origin"]) is None:
            return None
        if run_git(["-C", str(repo_dir), "reset", "--hard", "origin/HEAD"]) is None:
            return None
        return repo_dir

    print(f"cloning {REPO_URL} into {repo_dir}")
    if run_git(["clone", "--depth", "1", REPO_URL, str(repo_dir)]) is None:
        return None
    return repo_dir


def parse_labels(repo_dir):
    key_dir = repo_dir / "annotations" / "key"
    if not key_dir.is_dir():
        print(f"no annotations/key directory under {repo_dir}", file=sys.stderr)
        return []

    rows = []
    for path in sorted(key_dir.glob("*.key")):
        track_id = path.stem  # "1004923.LOFI.key" -> "1004923.LOFI"
        text = path.read_text(encoding="utf-8").strip()
        parts = text.split()

        if len(parts) != 2:
            print(f"skipping {path.name}: unparseable annotation {text!r}", file=sys.stderr)
            continue

        tonic_raw, mode = parts
        tonic = FLAT_TO_SHARP.get(tonic_raw, tonic_raw)

        if tonic not in VALID_TONICS or mode not in ("major", "minor"):
            print(f"skipping {path.name}: unrecognised key {text!r}", file=sys.stderr)
            continue

        rows.append((track_id, tonic, mode))

    return rows


def write_labels(rows, out_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("track_id\ttonic\tmode\n")
        for track_id, tonic, mode in rows:
            f.write(f"{track_id}\t{tonic}\t{mode}\n")


def fetch_audio_sample(repo_dir, out_dir, count):
    """Download up to `count` (capped at MAX_AUDIO_SAMPLE) audio previews as proof the download
    mechanism works, verifying each against the dataset's own md5. Saved under out_dir, outside
    the git repo -- audio is never committed."""
    count = min(count, MAX_AUDIO_SAMPLE)

    md5_dir = repo_dir / "md5"
    names = sorted(p.stem for p in md5_dir.glob("*.md5"))[:count]
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"\naudio sample: attempting {len(names)} file(s) into {out_dir}")

    ok = 0
    for name in names:
        expected = (md5_dir / f"{name}.md5").read_text(encoding="utf-8").strip()
        dest = out_dir / f"{name}.mp3"
        data = None
        used_url = None

        for url in (AUDIO_URL_PRIMARY.format(name=name), AUDIO_URL_BACKUP.format(name=name)):
            try:
                with urllib.request.urlopen(url, timeout=20) as resp:
                    data = resp.read()
                used_url = url
                break
            except (urllib.error.URLError, urllib.error.HTTPError) as exc:
                print(f"  {name}: {url} -> {exc}", file=sys.stderr)

        if data is None:
            print(f"  {name}: FAILED (no URL served the file)", file=sys.stderr)
            continue

        dest.write_bytes(data)
        digest = hashlib.md5(data).hexdigest()

        if digest == expected:
            ok += 1
            print(f"  {name}: OK ({len(data)} bytes) via {used_url}")
        else:
            print(f"  {name}: MD5 MISMATCH (expected {expected}, got {digest}) via {used_url}", file=sys.stderr)

    print(f"audio sample: {ok}/{len(names)} verified")


AUDIO_ALL_DIR = CACHE_DIR / "audio"
AUDIO_ALL_PAUSE_S = 0.2


def _download_one(name, expected_md5, dest):
    """Try the primary URL, then the backup, once each. Returns (ok, detail_str)."""
    for url in (AUDIO_URL_PRIMARY.format(name=name), AUDIO_URL_BACKUP.format(name=name)):
        try:
            with urllib.request.urlopen(url, timeout=30) as resp:
                data = resp.read()
        except (urllib.error.URLError, urllib.error.HTTPError) as exc:
            continue
        digest = hashlib.md5(data).hexdigest()
        if digest != expected_md5:
            return False, f"md5 mismatch via {url} (expected {expected_md5}, got {digest})"
        dest.write_bytes(data)
        return True, f"OK ({len(data)} bytes) via {url}"
    return False, "no URL served the file"


def fetch_audio_all(repo_dir, out_dir):
    """Download every track's audio, sequentially, politely paced, verifying against the repo's
    own md5. Failures are retried once, then recorded as missing and skipped -- never raised, so
    one bad track never aborts the whole run. Already-verified files (matching md5 already on
    disk) are skipped, so this is safe to re-run to pick up stragglers."""
    md5_dir = repo_dir / "md5"
    names = sorted(p.stem for p in md5_dir.glob("*.md5"))
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"audio (all): {len(names)} tracks -> {out_dir}")

    ok = 0
    skipped = 0
    failed = []

    for i, name in enumerate(names, 1):
        expected = (md5_dir / f"{name}.md5").read_text(encoding="utf-8").strip()
        dest = out_dir / f"{name}.mp3"

        if dest.exists():
            digest = hashlib.md5(dest.read_bytes()).hexdigest()
            if digest == expected:
                skipped += 1
                if i % 50 == 0 or i == len(names):
                    print(f"  [{i}/{len(names)}] {name}: already OK (skipped)")
                continue
            # stale/corrupt local file; fall through and re-download

        success, detail = _download_one(name, expected, dest)
        if not success:
            time.sleep(AUDIO_ALL_PAUSE_S)
            print(f"  [{i}/{len(names)}] {name}: attempt 1 failed ({detail}); retrying once", file=sys.stderr)
            success, detail = _download_one(name, expected, dest)

        if success:
            ok += 1
            if i % 25 == 0 or i == len(names):
                print(f"  [{i}/{len(names)}] {name}: {detail}")
        else:
            failed.append(name)
            print(f"  [{i}/{len(names)}] {name}: FAILED after retry ({detail})", file=sys.stderr)

        time.sleep(AUDIO_ALL_PAUSE_S)

    total_have = ok + skipped
    print(f"\naudio (all): {total_have}/{len(names)} present and verified "
          f"({ok} downloaded this run, {skipped} already present), {len(failed)} missing")
    if failed:
        missing_path = out_dir.parent / "audio_missing.txt"
        missing_path.write_text("\n".join(failed) + "\n", encoding="utf-8")
        print(f"missing track ids written to {missing_path}")
        for name in failed:
            print(f"  missing: {name}")

    return total_have, failed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo-dir", type=pathlib.Path, default=REPO_DIR,
                        help=f"cache location for the dataset clone (default: {REPO_DIR})")
    parser.add_argument("--out", type=pathlib.Path, default=OUT_TSV,
                        help=f"where to write the labels TSV (default: {OUT_TSV})")
    parser.add_argument("--no-update", action="store_true",
                        help="use an existing clone as-is; do not fetch/reset it")
    parser.add_argument("--audio-sample", type=int, default=0, metavar="N",
                        help=f"also download N audio previews as proof of mechanism, capped at "
                             f"{MAX_AUDIO_SAMPLE} regardless of N (default: 0, none)")
    parser.add_argument("--audio-out", type=pathlib.Path, default=CACHE_DIR / "audio_sample",
                        help="where sampled audio goes; always outside the git repo")
    parser.add_argument("--audio-all", action="store_true",
                        help=f"download every track's audio (604 files) into {AUDIO_ALL_DIR}, "
                             f"sequentially with a {AUDIO_ALL_PAUSE_S * 1000:.0f} ms pause between "
                             f"requests, verifying each against the repo's md5 and retrying a "
                             f"failure once before recording it as missing")
    args = parser.parse_args()

    repo_dir = clone_or_update(args.repo_dir, update=not args.no_update)
    if repo_dir is None:
        return 2

    rows = parse_labels(repo_dir)
    if not rows:
        print("No key annotations parsed; aborting.", file=sys.stderr)
        return 1

    write_labels(rows, args.out)
    print(f"wrote {len(rows)} labels to {args.out}")

    if args.audio_sample > 0:
        fetch_audio_sample(repo_dir, args.audio_out, args.audio_sample)

    if args.audio_all:
        _total_have, failed = fetch_audio_all(repo_dir, AUDIO_ALL_DIR)
        if failed:
            return 3

    return 0


if __name__ == "__main__":
    sys.exit(main())
