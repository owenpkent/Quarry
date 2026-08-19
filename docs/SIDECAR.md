# Quarry — The Sidecar

Companion to `ANALYSIS.md` §4.2, which argues for this design and records what it measures.
This document is the operating manual: what the sidecar is, how to set it up, how to run it,
and what its numbers and limits are.

The sidecar is an out-of-process transcription tier. Quarry launches it, hands it the take's
original-rate audio, and receives note events, velocities and sustain pedal back through the
same `Notes::Event` path the built-in engine uses, so the roll, post-processing and export
work unchanged. Out of process is load-bearing three ways: a CUDA fault is a failed job
rather than a dead DAW, the models load once and stay warm across takes, and it is the
licence boundary that keeps non-commercial weights out of Quarry's binary and distribution
(`ANALYSIS.md` §3.1). Basic Pitch remains the in-process default and the automatic fallback.

Measured through Quarry's own binary and bench, on real piano recordings
(`Tests/bench_corpus_maestro_real` / `Tests/bench_corpus_smd`):

| engine | onset F1 | velocity F1 | pedal F1 | notes |
| --- | --- | --- | --- | --- |
| kong | 0.981 / 0.944 | 0.958 / 0.889 | 0.827 / 0.861 | offsets sustain-extended natively, best under pedal |
| transkun | 0.985 / 0.953 | 0.977 / 0.925 | 0.895 (sparse CC64) | best onsets; key-release offsets |
| muscriptor | 0.849 / 0.822 | none emitted | none | the mixes engine: 0.433 BabySlakh, best there |
| CPU tier (for scale) | 0.775 / 0.760 | 0.275 | none | `ANALYSIS.md` §2 |

`sep+<engine>` (for example `sep+muscriptor`) runs Demucs htdemucs first and transcribes the
stems: drums dropped, notes tagged with their stem. Sized at +0.01 to +0.02 onset F1 on the
mix corpora as built; see the §4.2 caveats before reading that as final.

---

## 1. Setup

One virtualenv serves the sidecar and the bake-off tooling: `tools/bakeoff/.venv`, Python
3.12. To rebuild it from nothing:

```
py -m venv tools/bakeoff/.venv
tools/bakeoff/.venv/Scripts/python -m pip install numpy scipy mir_eval pretty_midi audioread librosa==0.9.2
tools/bakeoff/.venv/Scripts/python -m pip install torch --index-url https://download.pytorch.org/whl/cu128
tools/bakeoff/.venv/Scripts/python -m pip install piano-transcription-inference transkun muscriptor
tools/bakeoff/.venv/Scripts/python -m pip install --no-deps demucs julius lameenc sphn
```

Traps met once so nobody meets them twice:

- **The CUDA build must be forced.** The cu128 index tops out at torch 2.11, and pip does not
  consider `2.13.0+cpu` older than `2.11.0+cu128`, so an already-installed CPU torch silently
  survives a plain install. Use `--force-reinstall --no-deps "torch==2.11.0+cu128"` if a CPU
  build got there first. On Blackwell (sm_120) the CPU build is not a slow path, it is the
  only path, and a 30 s take costs 48 s instead of 6.
- **librosa is pinned to 0.9.2**: `piano_transcription_inference` calls a pre-lazy-loader API
  that librosa 1.0 removed, and it also needs `audioread` installed even though it does not
  declare it.
- **kong's checkpoint fetch shells out to `wget`**, which Windows does not have. Fetch it
  once by hand (Zenodo, ~165 MB) to the exact path the library expects:
  `~/piano_transcription_inference_data/note_F1=0.9677_pedal_F1=0.9186.pth`.
- **Demucs is installed `--no-deps`** so its loose torch requirement cannot downgrade the
  CUDA build; `julius`, `lameenc` and `sphn` are the three dependencies it genuinely needs.

**MuScriptor is gated.** Its weights are CC BY-NC 4.0 behind a HuggingFace licence gate, and
the acceptance is deliberately the user's, not Quarry's (`ANALYSIS.md` §3.1). In a browser,
accept at `https://huggingface.co/MuScriptor/muscriptor-medium` (and `-small` / `-large` if
wanted), create a Read token at `https://huggingface.co/settings/tokens`, then run
`powershell -ExecutionPolicy Bypass -File tools\bakeoff\set_hf_token.ps1` and paste the token
at its hidden prompt. Everything else works without any of this; only the muscriptor lanes
need it.

---

## 2. Running it

**In the app.** Two environment variables, read once at startup, documented where they are
read in `TranscriptionManager`:

```
set QUARRY_SIDECAR_CMD=c:\path\to\Quarry\tools\bakeoff\.venv\Scripts\python.exe c:\path\to\Quarry\tools\sidecar\quarry_sidecar.py serve
set QUARRY_SIDECAR_ENGINE=kong
```

Unset or empty means current behaviour, byte for byte. `QUARRY_SIDECAR_ENGINE` defaults to
`auto`, which currently means kong (the complete piano answer: onsets, offsets, velocity,
pedal); material-based routing arrives with the profiles of `STATS.md` §2. The client starts
lazily on the first transcription, lives for the session, and on any failure the take falls
back to Basic Pitch with a `DBG` log; one from-scratch retry is allowed before the sidecar is
given up for the session.

**On the bench.** Any corpus, any engine, scored identically to everything else:

```
Bench.exe Tests\bench_corpus_maestro_real --sidecar "<the same command>" --engine transkun
```

**One-shot, no server.** For a single file or a schema check:

```
tools\bakeoff\.venv\Scripts\python tools\sidecar\quarry_sidecar.py transcribe take.wav --engine kong --json-out out.json
```

The wire protocol (newline JSON over stdio, version 1) is specified in
`tools/sidecar/PROTOCOL.md`; `Lib/Sidecar/SidecarClient` is the C++ client. One detail worth
knowing before touching the client: `juce::ChildProcess` cannot write a child's stdin at all
(verified against the JUCE source), which is why the client drives native pipes directly.

---

## 3. Behaviour worth knowing

- **Velocity.** Sidecar-measured velocities (kong, transkun) bypass the CPU tier's
  `NoteVelocity` stage entirely. An engine that emits none (muscriptor) flows through that
  stage's documented "could not measure" fallback, exactly the case it was built for.
- **Pedal.** CC64 reaches exported MIDI through `MidiFileWriter`, unquantized even when note
  quantization is on (quantize-aware pedal mapping is future work). transkun's pedal is
  sparse but accurate (F1 0.895); a single-file check once concluded it emitted none, which
  is why the sidecar logs a warning rather than assuming.
- **Offsets are a convention, not a ranking.** Against pedal-extended ground truth, kong's
  sustain-extended offsets score 0.71 to 0.87 while transkun's key-release offsets collapse
  on pedalled material at 0.97+ onset F1. Pedal-extending transkun's offsets in the adapter
  is the obvious fix and has not been done yet.
- **The knobs.** Note Sens / Split Sens / Min Dur are Basic Pitch decoder parameters with no
  sidecar equivalent; on a sidecar take they skip re-decoding and only post-processing
  (scale snap, range, quantize) reruns.
- **The client is one-request-in-flight** and blocking on a background thread; the POSIX
  branch of `SidecarClient` is written but untested (Windows is the tested platform).
