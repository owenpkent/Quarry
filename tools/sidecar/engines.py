"""Transcription engine adapters shared by the sidecar (quarry_sidecar.py).

Refactored out of tools/bakeoff/run_bakeoff.py: that script is left completely untouched (it
still owns the mir_eval bake-off), this module owns the same three adapters for the sidecar so
neither side has to import across the tools/ boundary. The bake-off's own findings carry over
unchanged and are re-noted below where they affect the sidecar specifically:

- kong (piano_transcription_inference) is the complete piano answer: onsets, offsets, velocity
  and pedal (CC64), all first-class. See run_kong().
- transkun is driven as a subprocess (transkun.transcribe:main parses sys.argv directly and has
  no Python API); its pretrained weight ships inside the package, so there is no checkpoint
  download to wait on, but every call re-loads the model in a fresh process (see run_transkun()).
  Checked empirically for this sidecar: transkun's output MIDI carries zero CC64 events (see
  PROTOCOL.md) -- it reports key-release offsets, not sustain pedal.
- muscriptor needs detect_tempo=False. Its default beat-grid path moves the whole transcription
  onto a detected bar grid (BeatGrid.onset_delay), which is right for a DAW drop but was measured
  as a constant +1.0 s shift against ground truth here, collapsing scored F1 to 0.1 for timing
  reasons alone. Its "velocity" is a fixed placeholder (every note written at a constant 100,
  see muscriptor/tokenizer/notes.py), not a measurement, so the sidecar reports it as null.
- demucs (htdemucs) is an optional pre-stage, not an engine of its own: an engine name prefixed
  "sep+" (e.g. "sep+muscriptor") runs source separation first, then the named base engine on
  each remaining stem. See run_separated() and PROTOCOL.md's "sep+ engines" section. Demucs code
  is MIT; the htdemucs weights are research-use per the author, so this stays in the personal-tier
  sidecar and out of any distribution, consistent with docs/ANALYSIS.md §3.1.
"""

import pathlib
import subprocess
import sys
import tempfile

import pretty_midi
import torch


class EngineError(RuntimeError):
    """An engine-specific failure; the caller turns this into an ok:false response."""


# Engine-name prefix for the demucs separation pre-stage, e.g. "sep+muscriptor" -- see
# run_separated() and PROTOCOL.md. Not an entry in ENGINES itself: quarry_sidecar.Sidecar.transcribe
# strips the prefix to find the base engine's make()/run() pair and only adds the demucs step
# around it, so the base engine's own cached transcriber (see _transcriber_for) is shared between
# e.g. "kong" and "sep+kong" rather than loaded twice.
SEP_PREFIX = "sep+"


def default_device() -> str:
    return "cuda" if torch.cuda.is_available() else "cpu"


def available_engines() -> set[str]:
    """Which engine packages import cleanly in this interpreter. Cheap: this only imports the
    top-level package, it does not load any model weights (that happens lazily, per engine, on
    first transcribe -- see quarry_sidecar.Sidecar._transcriber_for). Also probes demucs; if it
    imports, every base engine found above gets a "sep+" twin added to the result (see SEP_PREFIX
    and run_separated())."""
    available = set()

    try:
        import piano_transcription_inference  # noqa: F401
        available.add("kong")
    except ImportError:
        pass

    try:
        import transkun  # noqa: F401
        available.add("transkun")
    except ImportError:
        pass

    try:
        import muscriptor  # noqa: F401
        available.add("muscriptor")
    except ImportError:
        pass

    try:
        import demucs.api  # noqa: F401
        available.update({f"{SEP_PREFIX}{base}" for base in available})
    except ImportError:
        pass

    return available


def _notes_from_pretty_midi(pm: "pretty_midi.PrettyMIDI", *, velocity_is_real: bool,
                            instrument_name_override: str | None = None):
    """Common MIDI -> (notes, pedal) parser for engines that hand back a MIDI file (transkun,
    muscriptor) rather than events directly (kong; see run_kong). instrument_name_override pins
    every note to one instrument string (transkun is piano-only); leave it None to derive the
    name per track from its General MIDI program (muscriptor is a general-instrument model)."""
    notes = []

    for instrument in pm.instruments:
        if instrument_name_override is not None:
            name = instrument_name_override
        elif instrument.is_drum:
            name = "drums"
        else:
            name = pretty_midi.program_to_instrument_name(instrument.program)

        for note in instrument.notes:
            notes.append({
                "onset": float(note.start),
                "offset": float(note.end),
                "pitch": int(note.pitch),
                "velocity": int(note.velocity) if velocity_is_real else None,
                "instrument": name,
            })

    notes.sort(key=lambda n: n["onset"])

    pedal = []
    for instrument in pm.instruments:
        for cc in instrument.control_changes:
            if cc.number == 64:
                pedal.append({"time": float(cc.time), "value": int(cc.value)})

    pedal.sort(key=lambda p: p["time"])

    return notes, pedal


# ---------------------------------------------------------------------------
# kong: piano_transcription_inference
# ---------------------------------------------------------------------------

def make_kong(device: str):
    from piano_transcription_inference import PianoTranscription

    return PianoTranscription(device=device)


def run_kong(transcriber, wav_path, _device: str, report=None):
    """Kong's own transcribe() hands back note/pedal events directly -- see
    piano_transcription_inference/inference.py: transcribe(audio, midi_path) returns
    {'est_note_events': [...], 'est_pedal_events': [...]}; midi_path=None skips the file write
    since the sidecar has no use for it. Pedal events are (onset_time, offset_time) hold
    intervals; kong's own write_events_to_midi turns each one into exactly the CC64 on/off pair
    reproduced below, so the sidecar's pedal list matches what a kong-written MIDI file would
    contain."""
    from piano_transcription_inference import load_audio
    from piano_transcription_inference import sample_rate as kong_sample_rate

    audio, _ = load_audio(str(wav_path), sr=kong_sample_rate, mono=True)

    if report:
        report("infer", "running kong inference")
    result = transcriber.transcribe(audio, None)
    if report:
        report("infer", "kong inference done")

    notes = []
    for ev in result["est_note_events"]:
        notes.append({
            "onset": float(ev["onset_time"]),
            "offset": float(ev["offset_time"]),
            "pitch": int(ev["midi_note"]),
            "velocity": int(ev["velocity"]),
            "instrument": "piano",
        })
    notes.sort(key=lambda n: n["onset"])

    pedal = []
    for ev in result["est_pedal_events"]:
        pedal.append({"time": float(ev["onset_time"]), "value": 127})
        pedal.append({"time": float(ev["offset_time"]), "value": 0})
    pedal.sort(key=lambda p: p["time"])

    return notes, pedal, []


# ---------------------------------------------------------------------------
# transkun
# ---------------------------------------------------------------------------

def make_transkun(_device: str):
    # No persistent object: transkun.transcribe:main parses sys.argv directly rather than
    # exposing a Python API, so it is driven the same way its own console script would be, as a
    # subprocess (see run_bakeoff.py's make_transkun_transcriber for the original note). That
    # also means "cached for the life of the process" does not fully apply to transkun: every
    # request re-loads its weights in a fresh subprocess. Documented in PROTOCOL.md.
    return None


def run_transkun(_transcriber, wav_path, device: str, report=None):
    with tempfile.TemporaryDirectory() as tmp:
        out_path = pathlib.Path(tmp) / "out.mid"

        if report:
            report("infer", "running transkun in a subprocess (weights reload every request)")
        subprocess.run(
            [sys.executable, "-m", "transkun.transcribe", str(wav_path), str(out_path),
             "--device", device],
            check=True, stdout=sys.stderr, stderr=sys.stderr,
        )
        if report:
            report("infer", "transkun inference done")

        pm = pretty_midi.PrettyMIDI(str(out_path))

    notes, pedal = _notes_from_pretty_midi(pm, velocity_is_real=True, instrument_name_override="piano")

    warnings = []
    if pedal:
        # Not expected (see the module docstring): flag it rather than silently changing shape.
        warnings.append(f"transkun MIDI unexpectedly contained {len(pedal)} CC64 event(s)")

    return notes, pedal, warnings


# ---------------------------------------------------------------------------
# muscriptor
# ---------------------------------------------------------------------------

def make_muscriptor(device: str):
    from muscriptor import TranscriptionModel

    return TranscriptionModel.load_model(device=device)


def run_muscriptor(transcriber, wav_path, _device: str, report=None):
    if report:
        report("infer", "running muscriptor inference")
    # detect_tempo=False keeps onsets in wall-clock time; see the module docstring for why.
    midi_bytes = transcriber.transcribe_to_midi(str(wav_path), detect_tempo=False)
    if report:
        report("infer", "muscriptor inference done")

    with tempfile.NamedTemporaryFile(suffix=".mid", delete=False) as handle:
        handle.write(midi_bytes)
        tmp_path = handle.name

    try:
        pm = pretty_midi.PrettyMIDI(tmp_path)
    finally:
        pathlib.Path(tmp_path).unlink(missing_ok=True)

    notes, pedal = _notes_from_pretty_midi(pm, velocity_is_real=False, instrument_name_override=None)

    return notes, pedal, []


ENGINES = {
    "kong": {"make": make_kong, "run": run_kong},
    "transkun": {"make": make_transkun, "run": run_transkun},
    "muscriptor": {"make": make_muscriptor, "run": run_muscriptor},
}


# ---------------------------------------------------------------------------
# demucs: optional source-separation pre-stage for "sep+<engine>" requests
# ---------------------------------------------------------------------------

# htdemucs's four stems; the drums stem is dropped before transcription (none of kong, transkun
# or muscriptor is a drum transcriber, and kong/transkun are piano-only besides).
DROPPED_STEMS = {"drums"}


def make_demucs_separator(device: str):
    """Loads htdemucs once. Shared by every "sep+*" engine for the life of the process -- see
    quarry_sidecar.Sidecar._separator_for -- so requesting both "sep+kong" and "sep+muscriptor"
    in the same session still only pays htdemucs's load cost once."""
    from demucs.api import Separator

    return Separator(model="htdemucs", device=device)


def run_separated(separator, base_run, base_transcriber, wav_path, device: str, report=None):
    """Runs htdemucs on wav_path, drops the drums stem, transcribes each remaining stem (bass,
    other, vocals) with the base engine's own run() function, tags every note's "instrument"
    field with the stem name it came from (overriding whatever the base engine would have
    guessed), and merges notes/pedal/warnings back into one response, sorted the same way a
    plain transcribe() call would sort them. Each request gets its own temp directory for the
    separated stem wavs, cleaned up before this function returns."""
    from demucs.api import save_audio

    if report:
        report("separate", "running demucs source separation")
    _original, stems = separator.separate_audio_file(wav_path)
    if report:
        report("separate", "separation done")

    notes = []
    pedal = []
    warnings = []

    stem_names = [name for name in stems if name not in DROPPED_STEMS]

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = pathlib.Path(tmp)

        for i, stem_name in enumerate(stem_names):
            if report:
                report("stem", f"transcribing {stem_name} stem ({i + 1}/{len(stem_names)})",
                       fraction=i / len(stem_names))

            stem_path = tmp_dir / f"{stem_name}.wav"
            save_audio(stems[stem_name], stem_path, samplerate=separator.samplerate)

            stem_notes, stem_pedal, stem_warnings = base_run(base_transcriber, stem_path, device, report=report)

            for note in stem_notes:
                note["instrument"] = stem_name

            notes.extend(stem_notes)
            pedal.extend(stem_pedal)
            warnings.extend(f"{stem_name}: {w}" for w in stem_warnings)

    notes.sort(key=lambda n: n["onset"])
    pedal.sort(key=lambda p: p["time"])

    return notes, pedal, warnings
