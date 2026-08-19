#!/usr/bin/env python3
"""Build the "SMD" bench corpus: real piano performances, real timing, real velocities, real
sustain pedal, AND real audio, not audio rendered from MIDI. This is a second real-material
corpus alongside tools/bench/make_real_corpus.py's rendered-MAESTRO one, but where that script
renders its window through FluidSynth from the original MIDI, SMD ships a genuine simultaneous
recording (two cardioid-condenser mics over a Yamaha Disklavier, feeding Steinberg Cubase) for
every performance, so there is no synthesis step: this script decodes the existing source audio
and slices the same 30s window straight out of it. Same <name>.wav / <name>.mid contract as every
other bench corpus.

Requires a local cache of the Saarland Music Data "MIDI-Audio Piano Music" set (v2, from Zenodo,
record 13753319 / concept DOI 10.5281/zenodo.10847280), unzipped so that:

    ~/.okstudio/smd-cache/SMD-piano_v2/midi/<stem>.mid
    ~/.okstudio/smd-cache/SMD-piano_v2/wav_44100_stereo/<stem>.wav

both exist for each of the 50 performances. There is no fetch_smd.py: the dataset is a single
~3.0 GiB zip, downloaded and unpacked by hand (`curl -L -o SMD-piano_v2.zip <url>`, then unzip
just the midi/ and wav_44100_stereo/ members, since the zip also carries 22.05kHz mono, CSV and
FluidSynth-rendered renditions of the same material that this script never reads).

What this does:

  1. Lists every performance stem with both a .mid and a wav_44100_stereo/.wav, sorted by
     filename. Deterministically (fixed-seed RNG, one draw) samples up to 20 of the 50 for the
     corpus, then re-sorts the chosen set so processing order is stable too.
  2. For each one, picks a 30-second excerpt with at least 20 note onsets in it (same
     pick_excerpt_start approach as make_real_corpus.py: random offsets tried first, a full
     grid-scan fallback so a qualifying window is always found if one exists).
  3. Ground truth <name>.mid: notes whose onset falls inside the excerpt window, original
     velocities, offsets pedal-extended (a note released while CC64 (sustain) is at or above 64
     keeps sounding, on paper, until the pedal next lifts) and clipped to the window end. Five of
     the 50 SMD performances carry no CC64 at all (the Disklavier simply wasn't pedalled); for
     those, build_ground_truth degrades automatically to a plain clip, because pedal_value_at
     reads MIDI's implicit "pedal up" default when there are no CC64 events to consult. Ground
     truth also carries the window's own CC64 stream: one synthetic event at t=0 with the pedal
     state at window start, plus every real CC64 event inside the window, times relative to
     window start; for the unpedalled five that is a single t=0 event at value 0.
  4. Audio <name>.wav: the corresponding real recording, read whole with soundfile, resampled to
     SAMPLE_RATE if its native rate ever isn't already 44100 (SMD's wav_44100_stereo tree always
     is, so this is a safety net rather than a path this corpus exercises), then sliced to the
     same window and written back out as 44100 Hz 16-bit PCM. No gain adjustment: this is the
     real recording's real level, not a render this script controls the loudness of.

Alignment: SMD's audio and MIDI are supposed to be synchronized to within a few milliseconds
(they were captured from the same Disklavier performance into the same Cubase project). Verified
by cross-correlating an audio RMS-delta onset-strength curve against a MIDI onset impulse train
for a handful of built pairs (see --spot-check below); the measured offset was under 20ms, so
AUDIO_OFFSET_SECONDS below is 0.0 and no correction is applied. If a future SMD release ever
drifts, rerun `--spot-check <name>` on a couple of built pairs and, if the offset is consistently
over 20ms, set AUDIO_OFFSET_SECONDS to the measured value (positive means the audio lags the
MIDI, i.e. slice the audio window that many seconds later so its content lines up with the
ground truth's timeline).

Usage:  tools/bench/.venv/Scripts/python.exe tools/bench/make_smd_corpus.py [output-dir]
        tools/bench/.venv/Scripts/python.exe tools/bench/make_smd_corpus.py --spot-check <name> [corpus-dir]
"""

import argparse
import bisect
import hashlib
import math
import os
import pathlib
import random
import sys
import wave

import numpy as np
import pretty_midi
import soundfile as sf

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE_DIR = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / ".okstudio" / "smd-cache"
SMD_ROOT = CACHE_DIR / "SMD-piano_v2"
MIDI_DIR = SMD_ROOT / "midi"
AUDIO_DIR = SMD_ROOT / "wav_44100_stereo"
DEFAULT_OUTPUT = REPO_ROOT / "Tests" / "bench_corpus_smd"

# Fixed so the corpus is exactly reproducible: same 20 performances, same 20 windows, every time,
# as long as the local SMD cache is the same v2 zip this script was written against.
SEED = 20260818

WINDOW_SECONDS = 30.0
MIN_ONSETS = 20
MAX_PIECES = 20
SAMPLE_RATE = 44100

# See "Alignment" in the module docstring. Positive means the audio's real content lags the
# MIDI's timeline by this many seconds; the correction (if any) is applied by sliding the audio
# slice window later by this amount, leaving the ground-truth MIDI timeline untouched.
AUDIO_OFFSET_SECONDS = 0.0


# --- pedal-state helpers (mirrors make_real_corpus.py) --------------------------------------


def merged_cc64(pm):
    events = sorted(
        ((cc.time, cc.value) for inst in pm.instruments for cc in inst.control_changes if cc.number == 64),
        key=lambda e: e[0],
    )
    return events


def pedal_value_at(cc64_events, times, t):
    idx = bisect.bisect_right(times, t)
    return cc64_events[idx - 1][1] if idx > 0 else 0


def next_pedal_release_after(cc64_events, times, t):
    idx = bisect.bisect_right(times, t)
    for et, ev in cc64_events[idx:]:
        if ev < 64:
            return et
    return None


# --- excerpt selection + ground truth (mirrors make_real_corpus.py) -------------------------


def pick_excerpt_start(notes_start_times, duration, rng):
    """Deterministically (given rng's state) find a WINDOW_SECONDS start with >= MIN_ONSETS
    onsets inside it. Tries random offsets first (so the corpus isn't just "the first 30s of
    every piece"), then falls back to a full grid scan so a qualifying window is always found
    if one exists.
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


def build_ground_truth(notes, cc64_events, cc64_times, start, end):
    gt_notes = []
    for n in notes:
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


def build_ground_truth_pedal(cc64_events, cc64_times, start, end):
    """The window's own CC64 stream, times relative to window start: one synthetic event at t=0
    carrying the pedal state at window start (so a window that opens mid-pedal is represented even
    though nothing actually fired inside it), plus every real CC64 event strictly inside the
    window. An event landing exactly on `start` is already captured by the synthetic t=0 one, so
    it is not duplicated.
    """
    gt_pedal = [(0.0, pedal_value_at(cc64_events, cc64_times, start))]
    for t, v in cc64_events:
        if start < t < end:
            gt_pedal.append((t - start, v))
    gt_pedal.sort(key=lambda e: e[0])
    return gt_pedal


def write_ground_truth_midi(path, gt_notes, gt_pedal=()):
    pm = pretty_midi.PrettyMIDI(resolution=10000, initial_tempo=120.0)
    inst = pretty_midi.Instrument(program=0, name="piano")
    for onset, offset, pitch, velocity in gt_notes:
        inst.notes.append(pretty_midi.Note(velocity=int(velocity), pitch=int(pitch), start=onset, end=offset))
    for time, value in gt_pedal:
        inst.control_changes.append(pretty_midi.ControlChange(number=64, value=int(value), time=time))
    pm.instruments.append(inst)
    pm.write(str(path))


# --- real-audio slicing (replaces make_real_corpus.py's FluidSynth render step) --------------


def load_audio(path):
    """Whole-file read as int16, resampled to SAMPLE_RATE if its native rate ever differs
    (SMD's wav_44100_stereo tree never does; this is only a safety net).
    """
    data, sr = sf.read(str(path), dtype="int16", always_2d=True)
    if sr != SAMPLE_RATE:
        import scipy.signal

        g = math.gcd(SAMPLE_RATE, sr)
        up, down = SAMPLE_RATE // g, sr // g
        resampled = scipy.signal.resample_poly(data.astype(np.float64), up, down, axis=0)
        data = np.clip(resampled, -32768, 32767).astype(np.int16)
        sr = SAMPLE_RATE
    return data, sr


def slice_window(samples, sr, start, window_seconds):
    start_frame = round(start * sr)
    want = round(window_seconds * sr)
    channels = samples.shape[1]

    window = samples[start_frame : start_frame + want]
    if len(window) < want:
        pad = np.zeros((want - len(window), channels), dtype=np.int16)
        window = np.concatenate([window, pad], axis=0)

    peak = int(np.abs(window).max()) if window.size else 0
    peak_dbfs = 20 * math.log10(peak / 32768.0) if peak > 0 else float("-inf")
    return window, channels, peak_dbfs


def write_wav(path, samples, channels):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(np.ascontiguousarray(samples, dtype="<i2").tobytes())


# --- alignment spot-check ---------------------------------------------------------------------


def onset_strength(mono, sr, hop_seconds):
    hop = max(1, round(hop_seconds * sr))
    n_frames = len(mono) // hop
    mono = mono[: n_frames * hop].astype(np.float64).reshape(n_frames, hop)
    rms = np.sqrt(np.mean(mono * mono, axis=1))
    delta = np.diff(rms, prepend=rms[0] if len(rms) else 0.0)
    return np.clip(delta, 0.0, None)


def onset_impulses(note_onsets, n_frames, hop_seconds):
    impulses = np.zeros(n_frames)
    for t in note_onsets:
        idx = round(t / hop_seconds)
        if 0 <= idx < n_frames:
            impulses[idx] += 1.0
    return impulses


def spot_check_alignment(wav_path, mid_path, max_lag_ms=300, hop_ms=10):
    """Cross-correlates an audio onset-strength curve (RMS-per-hop, positive frame-to-frame
    delta) against a MIDI onset impulse train at the same hop rate, over a +-max_lag_ms window,
    and returns the argmax lag in milliseconds: positive means the audio's content lags the
    MIDI's timeline by that much.
    """
    data, sr = sf.read(str(wav_path), dtype="int16", always_2d=True)
    mono = data.mean(axis=1)
    hop_seconds = hop_ms / 1000.0
    strength = onset_strength(mono, sr, hop_seconds)

    pm = pretty_midi.PrettyMIDI(str(mid_path))
    onsets = sorted(n.start for inst in pm.instruments for n in inst.notes)
    impulses = onset_impulses(onsets, len(strength), hop_seconds)

    a = strength - strength.mean()
    b = impulses - impulses.mean()
    max_lag = round(max_lag_ms / hop_ms)

    best_lag, best_score = 0, float("-inf")
    for lag in range(-max_lag, max_lag + 1):
        if lag >= 0:
            seg_a, seg_b = a[lag:], b[: len(a) - lag]
        else:
            seg_a, seg_b = a[: len(a) + lag], b[-lag:]
        n = min(len(seg_a), len(seg_b))
        if n <= 0:
            continue
        score = float(np.dot(seg_a[:n], seg_b[:n]))
        if score > best_score:
            best_score, best_lag = score, lag

    return best_lag * hop_ms


# --- driving it all ------------------------------------------------------------------------


def list_stems():
    midi_stems = {p.stem for p in MIDI_DIR.glob("*.mid")}
    audio_stems = {p.stem for p in AUDIO_DIR.glob("*.wav")}
    stems = sorted(midi_stems & audio_stems)
    missing_audio = sorted(midi_stems - audio_stems)
    missing_midi = sorted(audio_stems - midi_stems)
    if missing_audio:
        print(f"  note: {len(missing_audio)} MIDI file(s) with no matching audio, skipped: {missing_audio[:5]}")
    if missing_midi:
        print(f"  note: {len(missing_midi)} audio file(s) with no matching MIDI, skipped: {missing_midi[:5]}")
    return stems


def process_case(name, stem, rng, output_dir):
    midi_path = MIDI_DIR / f"{stem}.mid"
    wav_path = AUDIO_DIR / f"{stem}.wav"

    pm = pretty_midi.PrettyMIDI(str(midi_path))
    notes = sorted((n for inst in pm.instruments for n in inst.notes), key=lambda n: n.start)
    cc64_events = merged_cc64(pm)
    cc64_times = [t for t, _ in cc64_events]
    midi_duration = pm.get_end_time()

    audio_info = sf.info(str(wav_path))
    audio_duration = audio_info.frames / audio_info.samplerate
    duration = min(midi_duration, audio_duration)

    note_starts = [n.start for n in notes]
    start = pick_excerpt_start(note_starts, duration, rng)
    if start is None:
        print(f"  skipping {name}: no {WINDOW_SECONDS:.0f}s window with >= {MIN_ONSETS} onsets in {stem}")
        return None
    end = start + WINDOW_SECONDS

    gt_notes = build_ground_truth(notes, cc64_events, cc64_times, start, end)
    gt_pedal = build_ground_truth_pedal(cc64_events, cc64_times, start, end)
    write_ground_truth_midi(output_dir / f"{name}.mid", gt_notes, gt_pedal)

    samples, sr = load_audio(wav_path)
    window, channels, peak_dbfs = slice_window(samples, sr, start + AUDIO_OFFSET_SECONDS, WINDOW_SECONDS)
    write_wav(output_dir / f"{name}.wav", window, channels)

    return {
        "name": name,
        "notes": len(gt_notes),
        "has_pedal": len(cc64_events) > 0,
        "peak_dbfs": peak_dbfs,
        "start": start,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", nargs="?", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--spot-check", metavar="NAME", help="run the alignment spot-check on an already-built pair and exit")
    arguments = parser.parse_args()

    if arguments.spot_check:
        corpus_dir = arguments.output if arguments.output != DEFAULT_OUTPUT else DEFAULT_OUTPUT
        wav_path = corpus_dir / f"{arguments.spot_check}.wav"
        mid_path = corpus_dir / f"{arguments.spot_check}.mid"
        offset_ms = spot_check_alignment(wav_path, mid_path)
        print(f"{arguments.spot_check}: apparent audio-vs-MIDI offset = {offset_ms:.1f} ms")
        return 0

    if not MIDI_DIR.is_dir() or not AUDIO_DIR.is_dir():
        raise SystemExit(
            f"{MIDI_DIR} / {AUDIO_DIR} not found; download+unzip the SMD v2 archive "
            f"(midi/ and wav_44100_stereo/ members) into {SMD_ROOT} first"
        )

    arguments.output.mkdir(parents=True, exist_ok=True)
    print(f"midi dir:   {MIDI_DIR}")
    print(f"audio dir:  {AUDIO_DIR}")
    print(f"output:     {arguments.output}\n")

    stems = list_stems()
    print(f"found {len(stems)} SMD performances with both a .mid and a .wav")

    rng = random.Random(SEED)
    chosen = rng.sample(stems, min(MAX_PIECES, len(stems)))
    chosen.sort()

    results = []
    for idx, stem in enumerate(chosen, start=1):
        short_id = hashlib.sha1(stem.encode("utf-8")).hexdigest()[:4]
        name = f"smd{idx:02d}_{short_id}_{stem}"
        print(f"[{idx}/{len(chosen)}] {name}  <- {stem}")
        result = process_case(name, stem, rng, arguments.output)
        if result:
            results.append(result)
            pedal_note = "pedal" if result["has_pedal"] else "no pedal"
            print(f"    {result['notes']} gt notes, {pedal_note}, peak {result['peak_dbfs']:.1f} dBFS")

    print(f"\nwrote {len(results)} pairs to {arguments.output}")
    if results:
        note_counts = sorted(r["notes"] for r in results)
        mid = note_counts[len(note_counts) // 2]
        print(f"  notes per case: min {note_counts[0]}, median {mid}, max {note_counts[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
