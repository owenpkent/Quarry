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

import hashlib
import os
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


# ---------------------------------------------------------------------------
# Weight pinning (supply-chain hardening)
# ---------------------------------------------------------------------------
# Every engine loads third-party weights (kong, transkun and demucs's legacy path via torch.load,
# i.e. pickle). Quarry therefore resolves the weight files itself, checks them against the
# SHA-256 recorded below, and only then hands an explicit local path to the library so it never
# fetches "latest" or runs its own downloader. A mismatch or a missing file is an EngineError.
# Developers trying new weights can set QUARRY_SIDECAR_UNPINNED=1 to skip all of this (a warning
# is logged to stderr and the libraries behave as stock). To pin new weights, update the hash
# (and the HF revision) here.
#
# Each value is a list of (description, sha256) pairs; a sha256 of None means "not pinned yet"
# and is refused unless QUARRY_SIDECAR_UNPINNED=1.
PINNED_WEIGHTS = {
    # Zenodo record 4034264, fetched by piano_transcription_inference with wget and no checksum.
    "kong": [
        ("checkpoint", "c3fa9730725bf4a762f1c14bc80cd5986eacda01b026f5a4a2525cd607876141"),
    ],
    # Bundled inside the transkun 2.0.1 wheel (transkun/pretrained/).
    "transkun": [
        ("2.0.pt", "50a80010effc2a59ffcd068a95cd2b29bd7f23a27a3515bc3ccd209c89a3d44c"),
        ("2.0.conf", "d3d989214eb148230ee5df476d994dcde6af595904d3f968f1221d2e3bea5ac6"),
    ],
    # HF repo MUSCRIPTOR_REPO at MUSCRIPTOR_REVISION.
    "muscriptor": [
        ("model.safetensors", "ac80adbdf85d87231735fd948af7013441c0afced316c4e9067fd5d8a7fb97ec"),
        ("config.json", "43e13a70fc9ae0af36b7447c06f3eac2282daeb69d79c1ff840ede7fdaa26a3b"),
    ],
    # HF repo DEMUCS_REPO at DEMUCS_REVISION.
    "demucs": [
        ("955717e8.safetensors", "d9fa14133cfcc034a6758923bb3a8ca9f8dfd0b582134643bbf83f72c17576dd"),
        ("htdemucs.yaml", "239c445d0b14454d541ad8bd9bb271c9e536d267e8a4625208744cbb2e7bb66c"),
    ],
}

MUSCRIPTOR_REPO = "MuScriptor/muscriptor-medium"
MUSCRIPTOR_REVISION = "f32236969308476e01fd3aae67357de5feb05a2d"
DEMUCS_REPO = "adefossez/HTDemucs"
DEMUCS_REVISION = "bf35a81b663819a8255c8fefee17f9d812b786b5"
DEMUCS_SIGNATURE = "955717e8"

UNPINNED_ENV = "QUARRY_SIDECAR_UNPINNED"


def _unpinned(engine: str) -> bool:
    """True when the developer escape hatch is set; logs a warning each time it is consulted."""
    if os.environ.get(UNPINNED_ENV) == "1":
        print(f"WARNING: {UNPINNED_ENV}=1, skipping weight verification for {engine}",
              file=sys.stderr, flush=True)
        return True
    return False


def _sha256_file(path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _verify_sha256(path, expected, engine: str = "engine") -> None:
    """Raises EngineError unless path exists and hashes to expected (None = nothing pinned)."""
    refusal = f"weights for {engine} do not match the pinned hash; refusing to load"
    if expected is None:
        raise EngineError(f"{refusal} (no hash is pinned for {path})")
    if not pathlib.Path(path).is_file():
        raise EngineError(f"{refusal} (missing file: {path})")
    actual = _sha256_file(path)
    if actual != expected:
        raise EngineError(f"{refusal} ({path}: expected {expected}, got {actual})")


def _pinned_hash(engine: str, description: str):
    return dict(PINNED_WEIGHTS[engine])[description]


def _fetch_pinned_hf(engine: str, repo_id: str, revision: str, filename: str) -> pathlib.Path:
    """Downloads (or finds in the HF cache) filename at the pinned revision, verifies it, and
    returns the local path."""
    from huggingface_hub import hf_hub_download

    try:
        path = pathlib.Path(hf_hub_download(repo_id=repo_id, filename=filename, revision=revision))
    except Exception as exc:  # gated repo, no network, bad revision: all surface the same way
        raise EngineError(f"could not fetch {repo_id}@{revision[:8]}/{filename} for {engine}: {exc}") from exc
    _verify_sha256(path, _pinned_hash(engine, filename), engine)
    return path


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

    if _unpinned("kong"):
        return PianoTranscription(device=device)

    # The library's own default location (see PianoTranscription.__init__). Checked before the
    # constructor runs because a missing or short file would make it shell out to wget.
    checkpoint = (pathlib.Path.home() / "piano_transcription_inference_data"
                  / "note_F1=0.9677_pedal_F1=0.9186.pth")
    _verify_sha256(checkpoint, _pinned_hash("kong", "checkpoint"), "kong")

    return PianoTranscription(checkpoint_path=str(checkpoint), device=device)


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
    #
    # The returned dict (the "transcriber") carries the verified --weight/--conf paths; empty
    # means stock behaviour (QUARRY_SIDECAR_UNPINNED=1).
    if _unpinned("transkun"):
        return {}

    import importlib.util

    spec = importlib.util.find_spec("transkun")
    if spec is None or not spec.submodule_search_locations:
        raise EngineError("transkun is not installed")
    pretrained = pathlib.Path(list(spec.submodule_search_locations)[0]) / "pretrained"

    paths = {}
    for name in ("2.0.pt", "2.0.conf"):
        _verify_sha256(pretrained / name, _pinned_hash("transkun", name), "transkun")
        paths[name] = str(pretrained / name)

    return {"weight": paths["2.0.pt"], "conf": paths["2.0.conf"]}


def run_transkun(transcriber, wav_path, device: str, report=None):
    with tempfile.TemporaryDirectory() as tmp:
        out_path = pathlib.Path(tmp) / "out.mid"

        if report:
            report("infer", "running transkun in a subprocess (weights reload every request)")
        subprocess.run(
            [sys.executable, "-m", "transkun.transcribe", str(wav_path), str(out_path),
             "--device", device,
             *(["--weight", transcriber["weight"], "--conf", transcriber["conf"]] if transcriber else [])],
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

    if _unpinned("muscriptor"):
        return TranscriptionModel.load_model(device=device)

    # Fetch both files at the pinned commit ourselves, verify, then give load_model the local
    # path; it finds config.json next to the weights. This replaces its unpinned "medium"
    # (= latest main) download.
    weights = _fetch_pinned_hf("muscriptor", MUSCRIPTOR_REPO, MUSCRIPTOR_REVISION, "model.safetensors")
    _fetch_pinned_hf("muscriptor", MUSCRIPTOR_REPO, MUSCRIPTOR_REVISION, "config.json")

    return TranscriptionModel.load_model(weights, device=device)


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

    if _unpinned("demucs"):
        return Separator(model="htdemucs", device=device)

    import yaml
    from demucs.apply import BagOfModels
    from demucs.hf import load_safetensors_model
    from demucs.pretrained import REMOTE_ROOT, _parse_remote_files

    # Fetch the bag definition and its one model at the pinned commit, verify both, and build
    # the bag from those local files. Demucs's own get_model("htdemucs") would call
    # hf_hub_download with no revision (latest main) and fall back to torch.load(weights_only=
    # False) of the legacy AWS .th file; neither path is reachable this way.
    bag_path = _fetch_pinned_hf("demucs", DEMUCS_REPO, DEMUCS_REVISION, "htdemucs.yaml")
    weights_path = _fetch_pinned_hf("demucs", DEMUCS_REPO, DEMUCS_REVISION, f"{DEMUCS_SIGNATURE}.safetensors")

    with open(bag_path, encoding="utf-8") as handle:
        bag = yaml.safe_load(handle)
    if bag.get("models") != [DEMUCS_SIGNATURE]:
        raise EngineError("weights for demucs do not match the pinned hash; refusing to load "
                          f"(bag lists {bag.get('models')}, expected [{DEMUCS_SIGNATURE!r}])")
    # Consistency with the signature demucs ships in its own remote file list (the legacy .th
    # files encode a sha256 prefix in their name, but that hashes a different file format than
    # the safetensors verified above, so only the signature can be compared).
    if DEMUCS_SIGNATURE not in _parse_remote_files(REMOTE_ROOT / "files.txt"):
        raise EngineError("weights for demucs do not match the pinned hash; refusing to load "
                          f"(signature {DEMUCS_SIGNATURE} not in demucs's own model list)")

    class _PinnedSeparator(Separator):
        def _load_model(self):
            model = BagOfModels([load_safetensors_model(weights_path)],
                                bag.get("weights"), bag.get("segment"))
            model.eval()
            self._model = model
            self._audio_channels = model.audio_channels
            self._samplerate = model.samplerate

    return _PinnedSeparator(model="htdemucs", device=device)


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
