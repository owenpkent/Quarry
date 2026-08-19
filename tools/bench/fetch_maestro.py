#!/usr/bin/env python3
"""Fetch the raw material for the "rendered MAESTRO" bench corpus into a local cache.

Populates %USERPROFILE%/.okstudio/maestro-cache/ with three things, none of them checked into
the repo because together they run to several hundred megabytes:

  maestro-v3.0.0-midi.zip, unzipped to maestro-v3.0.0/
      Real piano performances (real timing, velocity, sustain pedal) as MIDI, from the Magenta
      MAESTRO v3 dataset, plus maestro-v3.0.0.csv (composer, split, duration, etc per performance).

  a FluidSynth Windows build, unzipped to fluidsynth/
      The command-line synth make_real_corpus.py shells out to for rendering.

  a piano SoundFont (.sf2)
      What FluidSynth renders through. A dedicated sampled piano beats a generic GM bank for
      this: General MIDI's piano patch is thin and synthetic next to a multi-velocity-layer
      sample set, and thin, synthetic audio is a worse test of a transcription engine than
      audio with real string and hammer character.

Nothing here is pip-installed; this script only needs the standard library.

Usage:  py tools/bench/fetch_maestro.py [--force]
"""

import argparse
import hashlib
import os
import pathlib
import sys
import tarfile
import urllib.request
import zipfile

CACHE_DIR = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / ".okstudio" / "maestro-cache"

# --- Pinned artifacts -------------------------------------------------------------------------
# Each URL below was live-checked and each SHA-256 was computed from the file it downloaded on
# 2026-08-18, the day this script was written. They are pinned so a later run notices instantly
# if the upstream file ever changes underneath us, instead of silently rendering the corpus from
# something other than what was verified. If a URL 404s, find the current one (the MAESTRO page
# at magenta.tensorflow.org/datasets/maestro, the fluidsynth GitHub releases page, or the
# freepats.zenvoid.org Piano section) and update both the URL and the hash together.

MAESTRO_MIDI_URL = "https://storage.googleapis.com/magentadata/datasets/maestro/v3.0.0/maestro-v3.0.0-midi.zip"
MAESTRO_MIDI_SHA256 = "70470ee253295c8d2c71e6d9d4a815189e35c89624b76d22fce5a019d5dde12c"

FLUIDSYNTH_URL = "https://github.com/FluidSynth/fluidsynth/releases/download/v2.6.0/fluidsynth-v2.6.0-win10-x64-cpp11.zip"
FLUIDSYNTH_SHA256 = "817262deacaa748edb3af6731dffe1766b00146790becfccc949a9f701e76681"

# Salamander Grand Piano, CC0, sampled across 16 velocity layers per note: the closest thing to
# a real recorded piano that is legally droppable into a cache directory. Ships as a .tar.xz
# containing one .sf2; SOUNDFONT_MEMBER_SUFFIX picks that member back out after extraction.
SOUNDFONT_URL = "https://freepats.zenvoid.org/Piano/SalamanderGrandPiano/SalamanderGrandPiano-SF2-V3+20200602.tar.xz"
SOUNDFONT_SHA256 = "15edb061d7ba60d58332f72dba8f8ce40988048cc703f935e6320f37d650e213"
SOUNDFONT_IS_TARBALL = True
SOUNDFONT_MEMBER_SUFFIX = ".sf2"

CHUNK = 1 << 20  # 1 MiB


def human(nbytes):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if nbytes < 1024:
            return f"{nbytes:.1f}{unit}"
        nbytes /= 1024
    return f"{nbytes:.1f}TiB"


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, dest, expected_sha256):
    """Stream url to dest via a .part file, renaming only once the download is complete.

    Skips entirely if dest already exists (and matches the pin, when one is set). Returns the
    sha256 actually observed, so callers can report it even on a first, unpinned run.
    """
    if dest.exists():
        if expected_sha256:
            observed = sha256_of(dest)
            if observed != expected_sha256:
                raise SystemExit(
                    f"cached file does not match pin: {dest}\n  expected {expected_sha256}\n  got      {observed}"
                )
        else:
            observed = sha256_of(dest)
        print(f"  already cached: {dest.name} ({human(dest.stat().st_size)})")
        return observed

    part = dest.with_suffix(dest.suffix + ".part")
    print(f"  downloading {url}")
    digest = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=60) as response, open(part, "wb") as out:
        total = int(response.headers.get("Content-Length", 0))
        written = 0
        while True:
            chunk = response.read(CHUNK)
            if not chunk:
                break
            out.write(chunk)
            digest.update(chunk)
            written += len(chunk)
            if total:
                print(f"\r  {human(written)} / {human(total)}", end="", flush=True)
    print()

    observed = digest.hexdigest()
    if expected_sha256 and observed != expected_sha256:
        part.unlink(missing_ok=True)
        raise SystemExit(f"downloaded file does not match pin: {url}\n  expected {expected_sha256}\n  got      {observed}")

    part.rename(dest)
    print(f"  saved {dest.name} ({human(dest.stat().st_size)}), sha256 {observed}")
    return observed


def unzip(archive, into, marker):
    """Extract archive into `into` unless a file matching the `marker` glob already exists
    somewhere under `into` (the zip may or may not nest its contents in a version-named folder,
    so this checks by pattern rather than by exact path).
    """
    if next(into.rglob(marker), None) is not None:
        print(f"  already unzipped: {archive.name}")
        return
    into.mkdir(parents=True, exist_ok=True)
    print(f"  unzipping {archive.name} -> {into}")
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(into)


def untar_member(archive, into, member_suffix, marker):
    """Extract the single member ending in member_suffix out of a tarball into `into`."""
    if marker.exists():
        print(f"  already extracted: {marker}")
        return marker
    into.mkdir(parents=True, exist_ok=True)
    print(f"  extracting {member_suffix} from {archive.name} -> {into}")
    with tarfile.open(archive, "r:xz") as tf:
        members = [m for m in tf.getmembers() if m.name.endswith(member_suffix)]
        if not members:
            raise SystemExit(f"no member ending in {member_suffix} found in {archive}")
        member = members[0]
        member.name = pathlib.Path(member.name).name  # flatten: drop the tar's internal folders
        tf.extract(member, into, filter="data")
    return into / member.name


def find_one(root, name):
    matches = list(root.rglob(name))
    if not matches:
        raise SystemExit(f"expected to find {name} under {root}, found nothing")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="re-download even if cached files are present")
    arguments = parser.parse_args()

    if arguments.force:
        for name in (
            "maestro-v3.0.0-midi.zip",
            "fluidsynth-v2.6.0-win10-x64-cpp11.zip",
            "SalamanderGrandPiano-SF2-V3+20200602.tar.xz",
        ):
            (CACHE_DIR / name).unlink(missing_ok=True)

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    print(f"cache: {CACHE_DIR}")

    print("\n[1/3] MAESTRO v3 MIDI")
    midi_zip = CACHE_DIR / "maestro-v3.0.0-midi.zip"
    download(MAESTRO_MIDI_URL, midi_zip, MAESTRO_MIDI_SHA256)
    unzip(midi_zip, CACHE_DIR, marker="maestro-v3.0.0.csv")
    maestro_root = CACHE_DIR / "maestro-v3.0.0"
    csv_path = maestro_root / "maestro-v3.0.0.csv"

    print("\n[2/3] FluidSynth (Windows x64)")
    fs_zip = CACHE_DIR / "fluidsynth-v2.6.0-win10-x64-cpp11.zip"
    download(FLUIDSYNTH_URL, fs_zip, FLUIDSYNTH_SHA256)
    fs_dir = CACHE_DIR / "fluidsynth"
    unzip(fs_zip, fs_dir, marker="fluidsynth.exe")
    fluidsynth_exe = find_one(fs_dir, "fluidsynth.exe")

    print("\n[3/3] Piano SoundFont (Salamander Grand Piano)")
    sf_tar = CACHE_DIR / "SalamanderGrandPiano-SF2-V3+20200602.tar.xz"
    download(SOUNDFONT_URL, sf_tar, SOUNDFONT_SHA256)
    sf_dir = CACHE_DIR / "soundfont"
    sf_marker = sf_dir / "SalamanderGrandPiano-V3+20200602.sf2"
    soundfont = untar_member(sf_tar, sf_dir, SOUNDFONT_MEMBER_SUFFIX, sf_marker)

    print("\ndone.")
    print(f"  maestro csv:  {csv_path}")
    print(f"  maestro root: {maestro_root}")
    print(f"  fluidsynth:   {fluidsynth_exe}")
    print(f"  soundfont:    {soundfont}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
