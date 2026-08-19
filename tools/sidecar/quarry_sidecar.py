#!/usr/bin/env python3
"""Quarry transcription sidecar. Talks over the newline-delimited JSON protocol documented in
PROTOCOL.md (protocol version 1); see that file for the wire format this implements.

Two modes:
  serve                         Long-running protocol server: reads one JSON request per line
                                 from stdin, writes one JSON response per line to stdout. stdout
                                 is protocol-only -- every engine log, progress bar and warning
                                 goes to stderr instead.
  transcribe WAV --engine X     One-shot, for manual testing: runs a single transcription and
                                 pretty-prints the same response JSON (see PROTOCOL.md) to
                                 --json-out, or to stdout if that is omitted. Not part of the
                                 wire protocol -- nothing reads this mode's stdout as NDJSON.

Usage:
  quarry_sidecar.py serve
  quarry_sidecar.py transcribe <wav> --engine kong|transkun|muscriptor|auto|sep+kong|sep+transkun|sep+muscriptor [--json-out PATH]

Engine adapters live in engines.py, next to this file (refactored out of
tools/bakeoff/run_bakeoff.py, which is untouched).
"""

import argparse
import contextlib
import json
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import engines  # noqa: E402  (path insert above must run first)

PROTOCOL_VERSION = 1

# "auto" is a routing alias, not an engine of its own. kong is the complete piano answer today
# (onsets, offsets, pedal, velocity, all first-class); material-based routing (mixes to
# muscriptor, etc.) arrives with profiles. Until then "auto" always resolves to "kong".
AUTO_ENGINE = "kong"


class Sidecar:
    """Owns the device choice, which engines are installed, and the lazily-built, per-process
    -cached transcriber for each engine actually used (see _transcriber_for)."""

    def __init__(self):
        self.device = engines.default_device()
        self.available = engines.available_engines()
        self._transcribers = {}
        self._separator = None  # demucs htdemucs, shared by every "sep+*" engine; see _separator_for

    def _transcriber_for(self, engine: str):
        if engine not in self._transcribers:
            make = engines.ENGINES[engine]["make"]
            with contextlib.redirect_stdout(sys.stderr):
                self._transcribers[engine] = make(self.device)
        return self._transcribers[engine]

    def _separator_for(self, device: str):
        if self._separator is None:
            with contextlib.redirect_stdout(sys.stderr):
                self._separator = engines.make_demucs_separator(device)
        return self._separator

    def transcribe(self, wav: str, engine: str) -> dict:
        resolved = AUTO_ENGINE if engine == "auto" else engine

        separated = resolved.startswith(engines.SEP_PREFIX)
        base = resolved[len(engines.SEP_PREFIX):] if separated else resolved

        if base not in engines.ENGINES:
            raise engines.EngineError(f"unknown engine: {engine!r}")
        if resolved not in self.available:
            raise engines.EngineError(f"engine not installed: {resolved!r}")

        wav_path = pathlib.Path(wav)
        if not wav_path.is_file():
            raise engines.EngineError(f"wav not found: {wav}")

        transcriber = self._transcriber_for(base)
        run = engines.ENGINES[base]["run"]

        started = time.time()
        with contextlib.redirect_stdout(sys.stderr):
            if separated:
                separator = self._separator_for(self.device)
                notes, pedal, warnings = engines.run_separated(separator, run, transcriber, wav_path, self.device)
            else:
                notes, pedal, warnings = run(transcriber, wav_path, self.device)
        elapsed = time.time() - started

        notes = sorted(notes, key=lambda n: n["onset"])
        pedal = sorted(pedal, key=lambda p: p["time"])
        warnings = list(warnings)
        if not notes:
            warnings.append("no notes detected")

        return {
            "engine": resolved,
            "elapsed_s": elapsed,
            "notes": notes,
            "pedal": pedal,
            "warnings": warnings,
        }


def _write(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def serve() -> int:
    sidecar = Sidecar()
    _write({
        "event": "ready",
        "protocol": PROTOCOL_VERSION,
        "engines": sorted(sidecar.available),
        "device": sidecar.device,
    })

    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue

        try:
            request = json.loads(line)
        except json.JSONDecodeError as exc:
            _write({"id": None, "ok": False, "error": f"invalid JSON: {exc}"})
            continue

        if not isinstance(request, dict):
            _write({"id": None, "ok": False, "error": "request must be a JSON object"})
            continue

        req_id = request.get("id")
        cmd = request.get("cmd")

        if cmd == "shutdown":
            break

        if cmd != "transcribe":
            _write({"id": req_id, "ok": False, "error": f"unknown cmd: {cmd!r}"})
            continue

        wav = request.get("wav")
        if not wav:
            _write({"id": req_id, "ok": False, "error": "missing required field: wav"})
            continue

        engine = request.get("engine", "auto")

        try:
            result = sidecar.transcribe(wav, engine)
            _write({"id": req_id, "ok": True, **result})
        except Exception as exc:  # noqa: BLE001 - any engine/IO failure becomes ok:false, not a crash
            print(f"[quarry_sidecar] transcribe failed for id={req_id!r}: {exc}", file=sys.stderr)
            _write({"id": req_id, "ok": False, "error": str(exc)})

    # Falls through to here either from an explicit "shutdown" or from EOF on stdin ending the
    # for-loop naturally; both exit cleanly.
    return 0


def transcribe_once(wav: pathlib.Path, engine: str, json_out) -> int:
    sidecar = Sidecar()

    try:
        result = sidecar.transcribe(str(wav), engine)
        response = {"id": "one-shot", "ok": True, **result}
        exit_code = 0
    except Exception as exc:  # noqa: BLE001 - report the same schema a serve-mode caller would see
        response = {"id": "one-shot", "ok": False, "error": str(exc)}
        exit_code = 1

    text = json.dumps(response, indent=2)

    if json_out:
        pathlib.Path(json_out).write_text(text + "\n", encoding="utf-8")
    else:
        print(text)

    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    subparsers.add_parser("serve", help="run the newline-delimited JSON protocol server")

    transcribe_parser = subparsers.add_parser("transcribe", help="one-shot transcription for testing")
    transcribe_parser.add_argument("wav", type=pathlib.Path)
    transcribe_parser.add_argument(
        "--engine", required=True,
        choices=["kong", "transkun", "muscriptor", "auto", "sep+kong", "sep+transkun", "sep+muscriptor"],
    )
    transcribe_parser.add_argument("--json-out", default=None, help="write the response JSON here instead of stdout")

    args = parser.parse_args()

    if args.mode == "serve":
        return serve()
    return transcribe_once(args.wav, args.engine, args.json_out)


if __name__ == "__main__":
    sys.exit(main())
