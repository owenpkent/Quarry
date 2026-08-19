#!/usr/bin/env python3
"""Build a stratified General MIDI bench corpus from a real MIDI collection: full band mixes,
not just piano. This is the third bench corpus, alongside make_corpus.py's synthetic piano and
make_real_corpus.py's rendered MAESTRO piano: neither of those ever puts a drum kit, a bass line,
or a horn section in front of the engine, and the plugin has to cope with all three in real use.

Requires: tools/bench/.venv (mido, pretty_midi, numpy) and the FluidSynth build fetch_maestro.py
already cached at %USERPROFILE%/.okstudio/maestro-cache/fluidsynth/. The GM SoundFont this script
needs is fetched here directly (see ensure_gm_soundfont below) rather than by fetch_maestro.py,
since that script's SoundFont is a dedicated piano patch unsuitable for anything else in GM.

What this does:

  1. Scan: deterministically samples up to SAMPLE_SIZE .mid files out of the whole collection
     (sorted full paths, one fixed-seed RNG, consumed in a fixed order throughout this script so
     the corpus is exactly reproducible run to run). Parses each with mido -- lightweight and
     tolerant enough to blow through thousands of arbitrary real-world files -- and classifies it
     by its *pitched* content: percussion (MIDI channel 10, index 9) is ignored everywhere in this
     step, both for counting programs and for counting notes, because a drum-only file or a
     drum-heavy file should not read as "few instruments" just because its kit sits on one channel.
     Corrupt files (anything mido can't parse) are skipped and only counted, not logged one by one.
  2. Select: CLASS_TARGETS per bucket (piano_solo, few_inst, full_mix), deterministic. For each,
     one WINDOW_SECONDS excerpt containing at least MIN_ONSETS pitched onsets, found the same way
     make_real_corpus.py finds its windows: random offsets first, then a full grid scan fallback.
  3. Ground truth <name>.mid: pitched notes only, onsets inside the window, per-channel pedal
     extension where CC64 applies (each pretty_midi Instrument here already corresponds to one
     source channel's notes, so "per channel" falls out of doing this per-instrument), clipped to
     the window end, velocities preserved. Percussion is excluded entirely.
  4. Render <name>.wav: the *full* excerpt, drums and all CCs and program changes included,
     through FluidR3_GM at 44100 Hz / 16-bit, rendered from t=0 through the window end (so pedal
     state and note history going into the window are honest) and then sliced, gain-searched the
     same way make_real_corpus.py does. The audio has drums in it; the ground truth does not --
     that mismatch is intentional, because an engine that hallucinates pitched notes onto a drum
     kit is a real failure mode, and this corpus is one of the only ones that can catch it.
  5. Names: gm_<class><i>_<slug>, <slug> a short sanitized stem of the source filename.

Usage:  tools/bench/.venv/Scripts/python.exe tools/bench/make_gm_corpus.py [output-dir]
"""

import argparse
import bisect
import hashlib
import math
import os
import pathlib
import random
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import wave

import mido
import pretty_midi

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE_DIR = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / ".okstudio" / "maestro-cache"
DEFAULT_OUTPUT = REPO_ROOT / "Tests" / "bench_corpus_gm"

# The user's MIDI collection. Strictly read-only: this script only ever opens files under it.
MIDI_COLLECTION_ROOT = pathlib.Path(r"C:\Users\owenp\Ableton\MIDI")

# Fixed so the corpus is exactly reproducible: same sample, same 24 picks, same 24 windows, every
# time, as long as the MIDI collection and the FluidSynth/SoundFont cache are unchanged. One RNG,
# created once in main() and threaded through every step below in a fixed order -- the file
# sample, the per-bucket pick, and each excerpt's window search -- rather than re-seeded per step.
SEED = 20260818

SAMPLE_SIZE = 3000
WINDOW_SECONDS = 30.0
MIN_ONSETS = 40
SAMPLE_RATE = 44100
TARGET_PEAK_DBFS = -1.0
# Rendered loud-to-quiet, same discipline as make_real_corpus.py, extended a couple of steps
# quieter: a full band plus drums stacks a lot more simultaneous energy than a solo piano, so the
# first gain candidate to clear it is often further down this list.
GAIN_CANDIDATES = [3.0, 2.0, 1.2, 0.7, 0.4, 0.22, 0.12, 0.06, 0.03]

PIANO_PROGRAM_MAX = 7  # GM programs 0-7: Acoustic/Electric/Honky-tonk Grand, E.Piano 1/2, Harpsichord, Clav
DRUM_CHANNEL = 9  # MIDI channel 10, zero-indexed

CLASS_ORDER = ("piano_solo", "few_inst", "full_mix")
CLASS_TARGETS = {"piano_solo": 8, "few_inst": 6, "full_mix": 10}
CLASS_MIN_NOTES = {"piano_solo": 100, "few_inst": 200, "full_mix": 400}

FLUIDSYNTH_EXE = next((CACHE_DIR / "fluidsynth").rglob("fluidsynth.exe"), None)
if FLUIDSYNTH_EXE is None:
    raise SystemExit(f"no fluidsynth.exe under {CACHE_DIR / 'fluidsynth'}; run tools/bench/fetch_maestro.py first")

# --- GM SoundFont: fetched here, not by fetch_maestro.py -------------------------------------
# fetch_maestro.py's SoundFont is Salamander Grand Piano, a dedicated piano patch with no GM bank
# behind it -- useless for rendering a bass line or a drum kit. FluidR3 GM (Frank Wen, MIT
# license) is a full General MIDI bank and is what every instrument program number in this corpus
# actually needs. Upstream ships it inside a Debian-packaging tarball together with a changelog
# and a readme; the tarball is what's pinned (it's the thing with a stable URL), and the .sf2
# member is re-pinned separately since it's the file that actually gets rendered through.
GM_SOUNDFONT_DIR = CACHE_DIR / "gm-soundfont"
GM_SOUNDFONT_TARBALL_URL = "https://ftp.osuosl.org/pub/musescore/soundfont/fluid-soundfont.tar.gz"
GM_SOUNDFONT_TARBALL_SHA256 = "c815769e44d86f1507b946a6c48c997c7f650699aea1ec4b11ba66e3415c26b9"
GM_SOUNDFONT_TARBALL_MEMBER = "FluidR3 GM2-2.SF2"
GM_SOUNDFONT_PATH = GM_SOUNDFONT_DIR / "FluidR3_GM.sf2"
GM_SOUNDFONT_SHA256 = "2ae766ab5c5deb6f7fffacd6316ec9f3699998cce821df3163e7b10a78a64066"

CHUNK = 1 << 20  # 1 MiB


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_gm_soundfont():
    """Return a path to a verified FluidR3_GM.sf2, downloading and extracting it into the cache
    on first use. Both the tarball and the extracted SoundFont are hash-pinned; either one not
    matching its pin is treated as "upstream changed underneath us" and stops the run rather than
    silently rendering through something unverified.
    """
    if GM_SOUNDFONT_PATH.exists():
        observed = sha256_of(GM_SOUNDFONT_PATH)
        if observed != GM_SOUNDFONT_SHA256:
            raise SystemExit(
                f"cached GM SoundFont does not match pin: {GM_SOUNDFONT_PATH}\n"
                f"  expected {GM_SOUNDFONT_SHA256}\n  got      {observed}"
            )
        return GM_SOUNDFONT_PATH

    GM_SOUNDFONT_DIR.mkdir(parents=True, exist_ok=True)
    tarball = GM_SOUNDFONT_DIR / "fluid-soundfont.tar.gz"

    if tarball.exists():
        observed = sha256_of(tarball)
        if observed != GM_SOUNDFONT_TARBALL_SHA256:
            raise SystemExit(
                f"cached GM SoundFont tarball does not match pin: {tarball}\n"
                f"  expected {GM_SOUNDFONT_TARBALL_SHA256}\n  got      {observed}"
            )
    else:
        print(f"  downloading {GM_SOUNDFONT_TARBALL_URL}")
        digest = hashlib.sha256()
        part = tarball.with_suffix(tarball.suffix + ".part")
        with urllib.request.urlopen(GM_SOUNDFONT_TARBALL_URL, timeout=60) as response, open(part, "wb") as out:
            while True:
                chunk = response.read(CHUNK)
                if not chunk:
                    break
                out.write(chunk)
                digest.update(chunk)
        observed = digest.hexdigest()
        if observed != GM_SOUNDFONT_TARBALL_SHA256:
            part.unlink(missing_ok=True)
            raise SystemExit(
                f"downloaded GM SoundFont tarball does not match pin: {GM_SOUNDFONT_TARBALL_URL}\n"
                f"  expected {GM_SOUNDFONT_TARBALL_SHA256}\n  got      {observed}"
            )
        part.rename(tarball)
        print(f"  saved {tarball.name}, sha256 {observed}")

    print(f"  extracting {GM_SOUNDFONT_TARBALL_MEMBER} -> {GM_SOUNDFONT_PATH}")
    with tarfile.open(tarball, "r:gz") as tf:
        member = tf.getmember(GM_SOUNDFONT_TARBALL_MEMBER)
        member.name = pathlib.Path(member.name).name
        tf.extract(member, GM_SOUNDFONT_DIR, filter="data")
    (GM_SOUNDFONT_DIR / GM_SOUNDFONT_TARBALL_MEMBER).rename(GM_SOUNDFONT_PATH)

    observed = sha256_of(GM_SOUNDFONT_PATH)
    if observed != GM_SOUNDFONT_SHA256:
        raise SystemExit(
            f"extracted GM SoundFont does not match pin: {GM_SOUNDFONT_PATH}\n"
            f"  expected {GM_SOUNDFONT_SHA256}\n  got      {observed}"
        )
    return GM_SOUNDFONT_PATH


# --- scan: one lightweight mido pass over the sampled files -----------------------------------


class ScanResult:
    __slots__ = ("path", "cls", "pitched_notes", "pitched_programs", "has_drums")

    def __init__(self, path, cls, pitched_notes, pitched_programs, has_drums):
        self.path = path
        self.cls = cls
        self.pitched_notes = pitched_notes
        self.pitched_programs = pitched_programs
        self.has_drums = has_drums


def classify_counts(pitched_programs, pitched_notes, has_drums):
    n = len(pitched_programs)
    if n == 0:
        return None
    if all(p <= PIANO_PROGRAM_MAX for p in pitched_programs):
        return "piano_solo" if pitched_notes >= CLASS_MIN_NOTES["piano_solo"] else None
    if n in (2, 3):
        return "few_inst" if pitched_notes >= CLASS_MIN_NOTES["few_inst"] else None
    if n >= 4 and has_drums:
        return "full_mix" if pitched_notes >= CLASS_MIN_NOTES["full_mix"] else None
    return None


def scan_one(path):
    """Parse one file with mido and classify it by pitched content. Returns a ScanResult, or
    None if the file is corrupt (unparseable) -- the caller counts those, it doesn't log them.
    """
    try:
        mid = mido.MidiFile(str(path))
        channel_program = [0] * 16
        pitched_programs = set()
        pitched_notes = 0
        has_drums = False
        for msg in mid:
            if msg.is_meta:
                continue
            if msg.type == "program_change":
                channel_program[msg.channel] = msg.program
            elif msg.type == "note_on" and msg.velocity > 0:
                if msg.channel == DRUM_CHANNEL:
                    has_drums = True
                else:
                    pitched_programs.add(channel_program[msg.channel])
                    pitched_notes += 1
    except Exception:
        return None

    cls = classify_counts(pitched_programs, pitched_notes, has_drums)
    return ScanResult(path, cls, pitched_notes, pitched_programs, has_drums)


def scan_sample(paths):
    results = []
    corrupt = 0
    for i, path in enumerate(paths):
        r = scan_one(path)
        if r is None:
            corrupt += 1
        else:
            results.append(r)
        if (i + 1) % 200 == 0 or i + 1 == len(paths):
            print(f"  scanned {i + 1}/{len(paths)} ({corrupt} corrupt so far)")
    return results, corrupt


# --- pedal-state helpers (per-instrument, i.e. per source channel) ----------------------------


def pedal_value_at(cc64_events, times, t):
    idx = bisect.bisect_right(times, t)
    return cc64_events[idx - 1][1] if idx > 0 else 0


def next_pedal_release_after(cc64_events, times, t):
    idx = bisect.bisect_right(times, t)
    for et, ev in cc64_events[idx:]:
        if ev < 64:
            return et
    return None


# --- excerpt selection + ground truth ----------------------------------------------------------


def pitched_instruments(pm):
    return [inst for inst in pm.instruments if not inst.is_drum]


def pick_excerpt_start(onset_times, duration, rng):
    """Same strategy as make_real_corpus.py: random offsets first, grid scan fallback, so windows
    aren't all just "the first 30s" but a qualifying one is still always found when one exists.
    """
    margin = 5.0
    lo, hi = margin, duration - WINDOW_SECONDS - margin
    if hi <= lo:
        lo, hi = 0.0, max(0.0, duration - WINDOW_SECONDS)

    def onset_count(start):
        end = start + WINDOW_SECONDS
        lo_i = bisect.bisect_left(onset_times, start)
        hi_i = bisect.bisect_left(onset_times, end)
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


def build_ground_truth(pm, start, end):
    """Pitched notes only (drums excluded by pitched_instruments), pedal-extended per instrument
    -- each pretty_midi Instrument here already holds only one source channel's events, so doing
    this per-instrument is exactly the "per-channel pedal extension" the corpus asks for.
    """
    gt_notes = []
    for inst in pitched_instruments(pm):
        cc64_events = sorted(
            ((cc.time, cc.value) for cc in inst.control_changes if cc.number == 64), key=lambda e: e[0]
        )
        cc64_times = [t for t, _ in cc64_events]
        for n in inst.notes:
            if not (start <= n.start < end):
                continue
            raw_offset = n.end
            if pedal_value_at(cc64_events, cc64_times, raw_offset) >= 64:
                release = next_pedal_release_after(cc64_events, cc64_times, raw_offset)
                eff_offset = release if release is not None else end
            else:
                eff_offset = raw_offset
            eff_offset = min(eff_offset, end)
            eff_offset = max(eff_offset, n.start + 0.005)
            gt_notes.append((n.start - start, eff_offset - start, n.pitch, n.velocity))
    gt_notes.sort(key=lambda t: (t[0], t[2]))
    return gt_notes


def write_ground_truth_midi(path, gt_notes):
    pm = pretty_midi.PrettyMIDI(resolution=10000, initial_tempo=120.0)
    inst = pretty_midi.Instrument(program=0, name="pitched")
    for onset, offset, pitch, velocity in gt_notes:
        inst.notes.append(pretty_midi.Note(velocity=int(velocity), pitch=int(pitch), start=onset, end=offset))
    pm.instruments.append(inst)
    pm.write(str(path))


# --- rendering -----------------------------------------------------------------------------


def build_render_source(pm, end):
    """A PrettyMIDI object holding the *entire* excerpt as originally scored, up to `end`: every
    instrument (drums included), every CC, every program change. This is deliberately not the
    ground-truth note set -- the audio carries the drum kit and all the other timbral information
    a real transcription has to sort a piano line out from, while the ground truth carries none of
    it. Getting that mismatch right is the point of this corpus.
    """
    out = pretty_midi.PrettyMIDI(resolution=10000, initial_tempo=120.0)
    for inst in pm.instruments:
        new_inst = pretty_midi.Instrument(program=inst.program, is_drum=inst.is_drum, name=inst.name)
        for n in inst.notes:
            if n.start < end:
                new_inst.notes.append(pretty_midi.Note(velocity=n.velocity, pitch=n.pitch, start=n.start, end=n.end))
        for cc in inst.control_changes:
            if cc.time < end:
                new_inst.control_changes.append(pretty_midi.ControlChange(number=cc.number, value=cc.value, time=cc.time))
        for pb in inst.pitch_bends:
            if pb.time < end:
                new_inst.pitch_bends.append(pretty_midi.PitchBend(pitch=pb.pitch, time=pb.time))
        out.instruments.append(new_inst)

    # Sentinel: forces the render to run through exactly `end` even when the last real event in
    # the window is earlier. Value 0 on an otherwise-empty instrument is harmless -- the window
    # gets sliced off exactly at `end` afterwards, so this event is never actually heard.
    sentinel = pretty_midi.Instrument(program=0, is_drum=False, name="sentinel")
    sentinel.control_changes.append(pretty_midi.ControlChange(number=64, value=0, time=end))
    out.instruments.append(sentinel)
    return out


def render_window(render_pm, soundfont, start, end, gain, work_dir):
    midi_path = work_dir / "render.mid"
    wav_path = work_dir / "render.wav"
    render_pm.write(str(midi_path))
    if wav_path.exists():
        wav_path.unlink()

    result = subprocess.run(
        [str(FLUIDSYNTH_EXE), "-ni", "-F", str(wav_path), "-r", str(SAMPLE_RATE), "-g", str(gain), str(soundfont), str(midi_path)],
        capture_output=True,
        text=True,
        timeout=180,
    )
    if result.returncode != 0 or not wav_path.exists():
        raise RuntimeError(f"fluidsynth failed (code {result.returncode}):\n{result.stdout}\n{result.stderr}")

    with wave.open(str(wav_path), "rb") as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        assert sampwidth == 2, f"expected 16-bit PCM, got {sampwidth * 8}-bit"
        frames = w.readframes(w.getnframes())

    import array

    samples = array.array("h")
    samples.frombytes(frames)

    start_frame = round(start * SAMPLE_RATE)
    window_frames = round(WINDOW_SECONDS * SAMPLE_RATE)
    start_idx = start_frame * channels
    want = window_frames * channels

    window = samples[start_idx : start_idx + want]
    if len(window) < want:
        window = window + array.array("h", [0]) * (want - len(window))

    peak = max((abs(s) for s in window), default=0)
    peak_dbfs = 20 * math.log10(peak / 32768.0) if peak > 0 else float("-inf")

    return window, channels, peak_dbfs


def write_wav(path, samples, channels):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.tobytes())


# --- naming ------------------------------------------------------------------------------------


def slugify(stem, max_len=24):
    s = re.sub(r"[^a-z0-9]+", "-", stem.lower()).strip("-")
    if len(s) > max_len:
        s = s[:max_len].rstrip("-")
    return s or "untitled"


# --- driving it all ------------------------------------------------------------------------


def process_candidate(name, path, rng, soundfont, work_dir, output_dir):
    try:
        pm = pretty_midi.PrettyMIDI(str(path))
    except Exception as exc:
        print(f"  skipping {name}: pretty_midi could not (re-)load {path.name}: {exc}")
        return None

    duration = pm.get_end_time()
    onset_times = sorted(n.start for inst in pitched_instruments(pm) for n in inst.notes)

    start = pick_excerpt_start(onset_times, duration, rng)
    if start is None:
        print(f"  skipping {name}: no {WINDOW_SECONDS:.0f}s window with >= {MIN_ONSETS} pitched onsets in {path.name}")
        return None
    end = start + WINDOW_SECONDS

    gt_notes = build_ground_truth(pm, start, end)
    render_pm = build_render_source(pm, end)

    chosen_gain = chosen_window = chosen_channels = chosen_peak = None
    for gain in GAIN_CANDIDATES:
        window, channels, peak_dbfs = render_window(render_pm, soundfont, start, end, gain, work_dir)
        chosen_gain, chosen_window, chosen_channels, chosen_peak = gain, window, channels, peak_dbfs
        if peak_dbfs <= TARGET_PEAK_DBFS:
            break
    else:
        print(f"  warning: {name} still at {chosen_peak:.1f} dBFS at the lowest gain candidate {chosen_gain}")

    write_wav(output_dir / f"{name}.wav", chosen_window, chosen_channels)
    write_ground_truth_midi(output_dir / f"{name}.mid", gt_notes)

    return {
        "name": name,
        "source": path,
        "gt_notes": len(gt_notes),
        "gain": chosen_gain,
        "peak_dbfs": chosen_peak,
        "start": start,
        "duration": duration,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", nargs="?", type=pathlib.Path, default=DEFAULT_OUTPUT)
    arguments = parser.parse_args()

    if not MIDI_COLLECTION_ROOT.is_dir():
        raise SystemExit(f"{MIDI_COLLECTION_ROOT} not found")

    arguments.output.mkdir(parents=True, exist_ok=True)

    print("GM SoundFont...")
    soundfont = ensure_gm_soundfont()
    print(f"fluidsynth: {FLUIDSYNTH_EXE}")
    print(f"soundfont:  {soundfont}")
    print(f"output:     {arguments.output}\n")

    rng = random.Random(SEED)

    print(f"listing {MIDI_COLLECTION_ROOT} ...")
    all_paths = sorted(MIDI_COLLECTION_ROOT.rglob("*.mid"), key=lambda p: str(p))
    print(f"  found {len(all_paths)} .mid files")

    sample_paths = rng.sample(all_paths, min(SAMPLE_SIZE, len(all_paths)))
    sample_paths.sort(key=lambda p: str(p))
    print(f"  sampled {len(sample_paths)} files (seed {SEED})\n")

    print("scanning sample with mido...")
    scan_results, corrupt = scan_sample(sample_paths)
    parsed = len(scan_results)
    print(f"\n  parsed {parsed}, corrupt {corrupt}")

    pools = {cls: sorted((r for r in scan_results if r.cls == cls), key=lambda r: str(r.path)) for cls in CLASS_ORDER}
    for cls in CLASS_ORDER:
        print(f"  class {cls}: {len(pools[cls])} candidates in sample")

    results = []
    with tempfile.TemporaryDirectory(prefix="gm_render_") as tmp:
        work_dir = pathlib.Path(tmp)
        for cls in CLASS_ORDER:
            target = CLASS_TARGETS[cls]
            pool = pools[cls]
            chosen = rng.sample(pool, target) if len(pool) > target else list(pool)
            if len(chosen) < target:
                print(f"  note: class {cls} has only {len(chosen)} candidates (< {target}); using all of them")
            chosen.sort(key=lambda r: str(r.path))

            for idx, cand in enumerate(chosen, start=1):
                slug = slugify(cand.path.stem)
                name = f"gm_{cls}{idx}_{slug}"
                print(f"[{cls}{idx}/{len(chosen)}] {name}  <- {cand.path}")
                result = process_candidate(name, cand.path, rng, soundfont, work_dir, arguments.output)
                if result:
                    result["cls"] = cls
                    results.append(result)
                    print(
                        f"    {result['gt_notes']} gt notes, gain {result['gain']}, "
                        f"peak {result['peak_dbfs']:.1f} dBFS, source duration {result['duration']:.1f}s"
                    )

    print(f"\nwrote {len(results)} pairs to {arguments.output}")
    for cls in CLASS_ORDER:
        count = sum(1 for r in results if r["cls"] == cls)
        print(f"  {cls}: {count}")
    if results:
        note_counts = sorted(r["gt_notes"] for r in results)
        mid = note_counts[len(note_counts) // 2]
        print(f"  gt notes per case: min {note_counts[0]}, median {mid}, max {note_counts[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
