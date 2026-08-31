# Quarry transcription sidecar protocol

Version 1. Implemented by `quarry_sidecar.py`; engine adapters live in `engines.py`.

## Transport

Newline-delimited JSON (NDJSON) over a pipe. In `serve` mode the sidecar reads requests from
stdin, one JSON object per line, and writes responses to stdout, one JSON object per line.

**stdout is protocol-only.** Every engine log, progress bar and warning goes to stderr instead.
kong and transkun both print to stdout on their own (checkpoint paths, device banners, `tqdm`
bars), so every call into a model, whether loading it or running it, is wrapped in
`contextlib.redirect_stdout(sys.stderr)`. transkun is invoked as a subprocess (see "Engines"
below), so that wrapping cannot reach across the process boundary; its subprocess is instead
launched with `stdout=sys.stderr` directly. A caller of `serve` mode should treat every stderr
line as unstructured and non-protocol: logging only, never parsed.

## Startup

On entering `serve` mode, before reading any request, the sidecar emits one line:

```json
{"event": "ready", "protocol": 1, "engines": ["kong", "muscriptor", "transkun"], "device": "cuda"}
```

- `engines` lists which engine packages import cleanly in this interpreter (sorted), not which
  ones have a model loaded yet -- see "Lazy loading" below. An engine absent from this list will
  fail any `transcribe` request that names it, with an `ok:false` response, not a crash. If
  `demucs` also imports cleanly, every base engine in the list gets a `"sep+"`-prefixed twin
  added too (e.g. `"sep+kong"`) -- see "sep+ engines" below. A client must tell an absent
  `engines` field apart from an empty one: absent means this sidecar does not report what it
  has, and the only honest thing to do with a request is send it and read the answer. Refusing
  every engine on a missing field turns any older or third-party `serve` into a sidecar with
  nothing installed.
- `device` is `"cuda"` if `torch.cuda.is_available()`, else `"cpu"`. It is the device every engine
  is loaded on for the life of the process; there is no per-request device override.

## Request

One JSON object per line:

```json
{"id": "<opaque string>", "cmd": "transcribe", "wav": "<absolute path>", "engine": "kong", "options": {}}
```

- `id`: caller-chosen, opaque, echoed back unchanged on the matching response. Required for a
  well-formed request; a malformed one gets `id: null` back (see "Malformed requests").
- `cmd`: `"transcribe"` or `"shutdown"` (see below). Anything else gets an `ok:false` response
  naming the unrecognized command; the process stays alive.
- `wav`: absolute path to a WAV file, read directly by the chosen engine's own loader.
- `engine`: one of `"kong"`, `"transkun"`, `"muscriptor"`, `"auto"`, or any of those three base
  names prefixed with `"sep+"` (`"sep+kong"`, `"sep+transkun"`, `"sep+muscriptor"`) -- see
  "sep+ engines" below. Only the names present in the `ready` line's `engines` list are usable;
  anything else gets `ok:false`.
  - `"auto"` currently always resolves to `"kong"`: kong is the complete piano answer (onsets,
    offsets, pedal and velocity, all first-class -- see "Engines"). Material-based routing (full
    mixes to muscriptor, etc.) arrives later with profiles; until then `"auto"` is a fixed alias,
    not a decision. `"auto"` never resolves to a `"sep+"` variant.
- `options`: reserved for future per-engine parameters (e.g. a profile name). Currently accepted
  but ignored; the sidecar never errors on unrecognized keys inside it.

## Response

On success, one JSON object per line, echoing the request's `id`:

```json
{
  "id": "<opaque string>",
  "ok": true,
  "engine": "kong",
  "elapsed_s": 4.231,
  "notes": [
    {"onset": 0.512, "offset": 0.98, "pitch": 60, "velocity": 87, "instrument": "piano"}
  ],
  "pedal": [
    {"time": 0.5, "value": 127},
    {"time": 1.2, "value": 0}
  ],
  "warnings": []
}
```

- `engine`: the engine that actually ran (i.e. `"kong"` when the request said `"auto"`).
- `elapsed_s`: wall-clock seconds for the transcription call itself (model already loaded;
  excludes first-use load time -- see "Lazy loading").
- `notes`: sorted by `onset` ascending.
  - `pitch`: MIDI note number, `int`. kong and transkun are piano-only, so in practice 21-108;
    muscriptor is a general-instrument model and is not clamped to that range.
  - `velocity`: `int` 0-127, or `null` when the engine does not produce a real one. muscriptor's
    tokenizer only encodes onset/offset (not a velocity measurement) and its MIDI writer emits a
    fixed placeholder value for every note (`velocity=100` in `muscriptor/tokenizer/notes.py`);
    reporting that constant as if it were measured would be worse than reporting `null`, so
    muscriptor notes always carry `velocity: null`. kong and transkun both report a real
    per-note velocity.
  - `instrument`: `"piano"` for kong and transkun (both are piano-only transcribers with no
    other output). For muscriptor, the General MIDI instrument name for that note's track
    program (`pretty_midi.program_to_instrument_name`), or `"drums"` for a drum track. For a
    `"sep+"` engine, the demucs stem name the note came from (`"bass"`, `"other"` or `"vocals"`)
    instead -- see "sep+ engines" below.
- `pedal`: CC64 (sustain) events, `{"time": <s>, "value": <int 0-127>}`, sorted by `time`.
  - **kong** emits pedal natively. Its `est_pedal_events` are (onset, offset) hold intervals;
    each interval becomes exactly the on/off pair kong's own MIDI writer would produce
    (`value: 127` at onset, `value: 0` at offset) -- see `engines.run_kong`.
  - **transkun**: empty. Checked empirically for this sidecar (30 s real-performance MAESTRO
    excerpt, GPU): transkun's output MIDI carried 0 CC64 events. Its model reports key-release
    offsets, not sustain pedal (an "omit pedal" comment sits next to the relevant loss code in
    transkun's own source). If some future transkun weight *did* emit CC64, the adapter still
    reports it rather than silently dropping it, and adds a `warnings` entry flagging the
    surprise.
  - **muscriptor**: empty in every case observed; nothing in its tokenizer represents a sustain
    pedal control change (see `engines.py` module docstring for what its "velocity" field
    actually means).
- `warnings`: non-fatal notices, e.g. `["no notes detected"]` when transcription succeeded but
  produced zero notes. `[]` when there is nothing to say.

## Error response

```json
{"id": "<opaque string>", "ok": false, "error": "<message>"}
```

Covers: an unrecognized `engine`, an engine whose package is not installed in this interpreter,
a `wav` path that does not exist, and any exception raised inside the engine adapter itself
(model failure, corrupt audio, etc.). **The process stays alive after an error** and keeps
reading requests; the exception and a short trace-context line are also written to stderr for
whoever is watching the sidecar's logs.

### Malformed requests

Not strictly part of the happy-path schema above, but specified here so a client knows what to
expect: a line that is not valid JSON, or valid JSON that is not an object, gets
`{"id": null, "ok": false, "error": "..."}` back (there is no `id` to echo). A well-formed object
missing `wav` gets `{"id": <echoed>, "ok": false, "error": "missing required field: wav"}`.

## Shutdown

`{"cmd": "shutdown"}` exits the process cleanly (no response line is sent for it). EOF on stdin
(the caller closes the pipe) also exits cleanly, with the same effect. Both are exit code 0.

## Lazy loading

Each engine's model loads on first use, on request, not at startup -- startup only checks which
engine *packages* import (see "Startup"). Once loaded, a transcriber is cached for the life of
the process, so a second request for the same engine skips the load entirely.

One exception, forced by transkun's own architecture: transkun has no Python API
(`transkun.transcribe:main` parses `sys.argv` directly), so it is driven as a subprocess exactly
as its own console script would be, and its weights are re-loaded inside a fresh subprocess on
*every* request. "Cached for the life of the process" does not apply to transkun's model weights,
only to the fact that the sidecar process itself does not need to re-import anything. This is the
same tradeoff `tools/bakeoff/run_bakeoff.py` already lives with; it does not change here.

## Engines

| engine     | pedal (CC64) | velocity      | instrument scope        |
|------------|--------------|---------------|--------------------------|
| kong       | yes, native  | real, per-note| piano only               |
| transkun   | no (checked) | real, per-note| piano only               |
| muscriptor | no           | `null` always | general (GM program name)|

## sep+ engines

An engine name prefixed `"sep+"` (e.g. `"sep+muscriptor"`) is not a fourth engine; it is the
demucs `htdemucs` separation model run as a pre-stage in front of one of the three base engines
above, still reached through the same `transcribe` request with no other change to the wire
format. Only present in the `ready` line's `engines` list when `demucs` imports cleanly in this
interpreter, and only for base engines that are themselves available (see "Startup").

What a `"sep+<base>"` request does, per call:

1. Runs `htdemucs` on `wav` (GPU when the sidecar's `device` is `"cuda"`; via demucs's own Python
   API, `demucs.api.Separator`, not its CLI), producing four stems: `drums`, `bass`, `other`,
   `vocals`.
2. Drops the `drums` stem -- none of the three base engines is a drum transcriber.
3. Writes each remaining stem to its own temp wav in a per-request temp directory, and runs
   `<base>`'s own `transcribe` logic on each stem wav independently.
4. Tags every note's `instrument` field with the stem name it came from (`"bass"`, `"other"` or
   `"vocals"`), overriding whatever the base engine would otherwise have reported there (e.g.
   kong and transkun would otherwise always say `"piano"`).
5. Merges the three stems' notes into one list sorted by `onset`, and their `pedal` lists into
   one list sorted by `time`, exactly as a plain (non-`sep+`) response is sorted. `warnings` from
   each stem are kept, prefixed with the stem name (e.g. `"bass: no notes detected"`).
6. Deletes the per-request temp directory before the response is written.

`elapsed_s` covers the whole pipeline for a `sep+` request: separation plus all per-stem
transcription calls, not just the last one.

`htdemucs` itself loads once per sidecar process and is shared by every `"sep+*"` engine used in
that session (`Sidecar._separator_for`), the same lazy-load-then-cache behavior described in
"Lazy loading" for the three base engines; the base engine's own transcriber is also shared
between e.g. `"kong"` and `"sep+kong"` rather than loaded twice.

**Licensing.** demucs's code is MIT; the `htdemucs` weights are research-use per the author. Per
`docs/ANALYSIS.md` §3.1 this keeps separation in the same personal-tier, arms-length-process
posture as muscriptor's CC BY-NC weights: installed by the user themselves, out of any
distribution.

## One-shot CLI (not part of the wire protocol)

`quarry_sidecar.py transcribe <wav> --engine kong|transkun|muscriptor|auto|sep+kong|sep+transkun|sep+muscriptor [--json-out PATH]`
runs a single transcription and pretty-prints the same response JSON shown above to `--json-out`
(if given) or to stdout. This mode is for manual testing only; nothing reads its stdout as NDJSON,
so ordinary Python tracebacks and `--json-out`-less pretty JSON are both fine there. A `sep+`
engine works the same as in `serve` mode (see "sep+ engines" above); this mode does not skip or
shortcut the separation step.
