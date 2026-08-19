#!/usr/bin/env python3
"""Build the "real MAESTRO audio" bench corpus: the exact same performances, windows, and
ground-truth <name>.mid files as make_real_corpus.py picks, but the <name>.wav is a sample-
accurate slice of MAESTRO's own Disklavier recording, not a FluidSynth render of the same MIDI.
This is the real-audio twin of Tests/bench_corpus_maestro/: same names, same ground truth,
different (real, not synthesized) waveform, so a bake-off run over this corpus measures how an
engine does on an actual recording rather than a clean render of one.

Reuses make_real_corpus.select_and_process() in manifest mode (render=False) to reproduce the
identical seed walk (same SEED, same survey/stratify/rng.sample/excerpt-pick order) and get, for
each case: its name, its window start in the source performance, and the MAESTRO audio_filename
that recording lives at. That call also writes the ground-truth <name>.mid files directly to
`output`, byte-identical to Tests/bench_corpus_maestro/*.mid (verified once when this script was
written; re-verified here on every run against that directory, when present).

Requires: the local MAESTRO v3 cache tools/bench/fetch_maestro.py fills (MIDI tree + metadata
CSV) for the selection walk, and the full maestro-v3.0.0.zip (see MAESTRO_ZIP below, not fetched
by fetch_maestro.py: it's 101 GB) for the audio. Only the ~22 needed member .wav files are ever
extracted from the zip, into --extract-dir, not the whole archive.

Usage:  tools/bench/.venv/Scripts/python.exe tools/bench/make_real_audio_corpus.py [output-dir]
"""

import argparse
import pathlib
import sys
import wave
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_real_corpus as mrc

REPO_ROOT = mrc.REPO_ROOT
CACHE_DIR = mrc.CACHE_DIR
DEFAULT_OUTPUT = REPO_ROOT / "Tests" / "bench_corpus_maestro_real"
RENDERED_CORPUS = REPO_ROOT / "Tests" / "bench_corpus_maestro"
DEFAULT_ZIP = CACHE_DIR / "maestro-v3.0.0.zip"
DEFAULT_EXTRACT_DIR = CACHE_DIR / "audio-extract"

WINDOW_SECONDS = mrc.WINDOW_SECONDS
ZIP_ROOT = "maestro-v3.0.0"


def extract_audio(results, zip_path, extract_dir):
    extract_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        names = set(z.namelist())
        for r in results:
            member = f"{ZIP_ROOT}/{r['audio_filename']}"
            if member not in names:
                raise SystemExit(f"audio member not found in {zip_path}: {member}")
            dest = extract_dir / pathlib.Path(r["audio_filename"]).name
            info = z.getinfo(member)
            if dest.exists() and dest.stat().st_size == info.file_size:
                print(f"  already extracted: {dest.name}")
                continue
            with z.open(member) as src, open(dest, "wb") as out:
                out.write(src.read())
            print(f"  extracted: {dest.name} ({info.file_size / (1024 * 1024):.1f} MiB)")


def slice_one(result, extract_dir, output_dir):
    src_path = extract_dir / pathlib.Path(result["audio_filename"]).name
    with wave.open(str(src_path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        rate = w.getframerate()
        n_frames = w.getnframes()
        assert sampwidth == 2, f"{src_path.name}: expected 16-bit PCM, got {sampwidth * 8}-bit"

        start_frame = round(result["start"] * rate)
        want_frames = round(WINDOW_SECONDS * rate)
        w.setpos(min(start_frame, n_frames))
        frames = w.readframes(min(want_frames, max(0, n_frames - start_frame)))

    import array

    samples = array.array("h")
    samples.frombytes(frames)
    want_samples = want_frames * channels
    if len(samples) < want_samples:
        samples = samples + array.array("h", [0]) * (want_samples - len(samples))

    dest = output_dir / f"{result['name']}.wav"
    with wave.open(str(dest), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.tobytes())

    peak = max((abs(s) for s in samples), default=0)
    peak_dbfs = 20 * __import__("math").log10(peak / 32768.0) if peak > 0 else float("-inf")
    duration = len(samples) / channels / rate
    return rate, channels, duration, peak_dbfs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", nargs="?", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--zip", type=pathlib.Path, default=DEFAULT_ZIP, help="path to maestro-v3.0.0.zip")
    parser.add_argument("--extract-dir", type=pathlib.Path, default=DEFAULT_EXTRACT_DIR)
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help=f"skip the byte-identical ground-truth cross-check against {RENDERED_CORPUS}",
    )
    args = parser.parse_args()

    if not args.zip.exists():
        raise SystemExit(f"{args.zip} not found")

    args.output.mkdir(parents=True, exist_ok=True)

    print("running the deterministic MAESTRO selection walk (manifest mode, no FluidSynth)...")
    results = mrc.select_and_process(args.output, render=False)

    if not args.skip_verify and RENDERED_CORPUS.exists():
        mismatches = []
        for r in results:
            rendered_mid = RENDERED_CORPUS / f"{r['name']}.mid"
            new_mid = args.output / f"{r['name']}.mid"
            if rendered_mid.exists() and rendered_mid.read_bytes() != new_mid.read_bytes():
                mismatches.append(r["name"])
        if mismatches:
            raise SystemExit(f"ground truth mismatch vs {RENDERED_CORPUS} for: {mismatches}")
        print(f"  verified {len(results)} ground-truth mids byte-identical to {RENDERED_CORPUS}\n")

    print(f"extracting {len(results)} audio files from {args.zip}...")
    extract_audio(results, args.zip, args.extract_dir)

    print(f"\nslicing {len(results)} x {WINDOW_SECONDS:.0f}s windows into {args.output}...")
    for r in results:
        rate, channels, duration, peak_dbfs = slice_one(r, args.extract_dir, args.output)
        print(f"  {r['name']}: {rate} Hz, {channels}ch, {duration:.3f}s, peak {peak_dbfs:.1f} dBFS")

    print(f"\nwrote {len(results)} real-audio pairs to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
