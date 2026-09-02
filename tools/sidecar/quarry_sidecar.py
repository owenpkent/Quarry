#!/usr/bin/env python3
"""Quarry transcription sidecar. Talks over the newline-delimited JSON protocol documented in
PROTOCOL.md (protocol version 2); see that file for the wire format this implements.

Two modes:
  serve                         Long-running protocol server: reads one JSON request per line
                                 from stdin, writes one JSON response per line to stdout. stdout
                                 is protocol-only -- every engine log, progress bar and warning
                                 goes to stderr instead. While a request is in flight, zero or
                                 more "stage" events may be written to stdout ahead of the
                                 response, reporting progress; see PROTOCOL.md.
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

PROTOCOL_VERSION = 2

# The protocol stream, taken before anything can redirect it.
#
# Engine libraries print freely to stdout ("Segment 15 / 25"), so transcribe() and download() run
# them under contextlib.redirect_stdout(sys.stderr) to keep that chatter off the wire. But that
# redirect rebinds sys.stdout for everything running inside the block -- including the sidecar's
# own stage events, which report() emits from deep inside the engine call. Those were landing on
# stderr as raw JSON, so the caller never saw the `infer`, `separate` or `stem` stages at all:
# exactly the slow ones a progress readout exists for. Writing protocol lines here instead means
# the wire is the wire no matter who is redirecting what.
_PROTOCOL_OUT = sys.stdout

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
        # Wall-clock start of whatever request is currently being served, so stage() can report
        # "t" (seconds since the request started) without every caller threading it through by
        # hand. Only one request is ever in flight (see serve()'s loop), so instance state is
        # enough; set at the top of transcribe()/download().
        self._request_started = None

    def stage(self, req_id, slug: str, text: str, fraction=None) -> None:
        """Writes one stage event to stdout -- see PROTOCOL.md's "Stage events" section. Callers
        never call this directly during a transcribe/download; they go through the `report`
        closure serve() builds per-request (see _report_for), which is what threads req_id
        through without every engine function having to know it."""
        elapsed = time.time() - self._request_started if self._request_started is not None else 0.0
        event = {"event": "stage", "id": req_id, "stage": slug, "text": text, "t": elapsed}
        if fraction is not None:
            event["fraction"] = fraction
        _write(event)

    def _transcriber_for(self, engine: str, report=None):
        if engine not in self._transcribers:
            if report:
                report("load-model", f"loading {engine} model")
            make = engines.ENGINES[engine]["make"]
            with contextlib.redirect_stdout(sys.stderr):
                self._transcribers[engine] = make(self.device)
            if report:
                report("load-model", f"{engine} model loaded")
        return self._transcribers[engine]

    def _separator_for(self, device: str, report=None):
        if self._separator is None:
            if report:
                report("load-model", "loading demucs (htdemucs) separator")
            with contextlib.redirect_stdout(sys.stderr):
                self._separator = engines.make_demucs_separator(device)
            if report:
                report("load-model", "demucs separator loaded")
        return self._separator

    def transcribe(self, wav: str, engine: str, report=None) -> dict:
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

        started = time.time()
        self._request_started = started
        if report:
            report("received", f"received transcribe request: engine={resolved} device={self.device}")

        transcriber = self._transcriber_for(base, report=report)
        run = engines.ENGINES[base]["run"]

        with contextlib.redirect_stdout(sys.stderr):
            if separated:
                separator = self._separator_for(self.device, report=report)
                notes, pedal, warnings = engines.run_separated(separator, run, transcriber, wav_path, self.device,
                                                                report=report)
            else:
                notes, pedal, warnings = run(transcriber, wav_path, self.device, report=report)
        elapsed = time.time() - started

        if report:
            report("post", "sorting notes and pedal events")
        notes = sorted(notes, key=lambda n: n["onset"])
        pedal = sorted(pedal, key=lambda p: p["time"])
        warnings = list(warnings)
        if not notes:
            warnings.append("no notes detected")

        if report:
            report("done", f"{len(notes)} note(s) in {elapsed:.2f}s")

        return {
            "engine": resolved,
            "elapsed_s": elapsed,
            "notes": notes,
            "pedal": pedal,
            "warnings": warnings,
        }

    def download(self, url: str, out_dir: str, report=None) -> dict:
        """Fetches url's audio with yt-dlp's Python API and writes a wav into out_dir. Uses the
        API rather than shelling out so progress and log lines can be captured and re-emitted as
        stage events / stderr lines instead of yt-dlp's own terminal output. See PROTOCOL.md's
        "download" section. Exists for the developer's own workflow (pulling reference audio in
        without leaving the sidecar); it is not part of the shipped plugin."""
        try:
            import yt_dlp
        except ImportError as exc:
            raise engines.EngineError("yt-dlp is not installed; pip install yt-dlp into the sidecar venv") from exc

        started = time.time()
        self._request_started = started
        if report:
            report("download", f"downloading {url}")

        out_path = pathlib.Path(out_dir)
        out_path.mkdir(parents=True, exist_ok=True)

        class _ForwardingLogger:
            """Every line yt-dlp would otherwise print goes to stderr instead, one line at a
            time, per PROTOCOL.md's transport rule (stdout is protocol-only)."""

            @staticmethod
            def debug(msg):
                print(msg, file=sys.stderr)

            @staticmethod
            def info(msg):
                print(msg, file=sys.stderr)

            @staticmethod
            def warning(msg):
                print(msg, file=sys.stderr)

            @staticmethod
            def error(msg):
                print(msg, file=sys.stderr)

        # Throttled to ~4/s: yt-dlp's own hook fires far more often than a "download" stage
        # event is worth writing to the protocol stream.
        last_emit = [0.0]

        def _progress_hook(d):
            if not report or d.get("status") != "downloading":
                return

            now = time.time()
            if now - last_emit[0] < 0.25:
                return
            last_emit[0] = now

            downloaded = d.get("downloaded_bytes") or 0
            total = d.get("total_bytes") or d.get("total_bytes_estimate")
            fraction = (downloaded / total) if total else None
            text = f"downloading... {downloaded}/{total} bytes" if total else f"downloading... {downloaded} bytes"
            report("download", text, fraction)

        # Filled in by _postprocessor_hook once FFmpegExtractAudio actually finishes, rather than
        # guessed from the title/outtmpl: yt-dlp is the only one that knows the real extension and
        # any sanitizing it applied to the filename.
        final_path = [None]

        def _postprocessor_hook(d):
            # yt-dlp runs more postprocessors than the one we asked for (e.g. a MoveFiles step
            # after ours), so this only reports the "extract-audio" stage for our own; every
            # postprocessor's "finished" status is still watched for the final path, though,
            # since a later step (MoveFiles) is what actually leaves the file at its final name.
            if d.get("postprocessor") == "ExtractAudio" and d.get("status") == "started" and report:
                report("extract-audio", "extracting audio to wav")

            if d.get("status") == "finished":
                info = d.get("info_dict") or {}
                if info.get("filepath"):
                    final_path[0] = info["filepath"]

        ydl_opts = {
            "format": "bestaudio/best",
            "noplaylist": True,
            "outtmpl": str(out_path / "%(title)s.%(ext)s"),
            "windowsfilenames": True,
            "quiet": True,
            "logger": _ForwardingLogger(),
            "progress_hooks": [_progress_hook],
            "postprocessor_hooks": [_postprocessor_hook],
            "postprocessors": [{"key": "FFmpegExtractAudio", "preferredcodec": "wav"}],
        }

        with contextlib.redirect_stdout(sys.stderr):
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info(url, download=True)

        if final_path[0] is None:
            # Fallback for a yt-dlp version whose postprocessor_hooks info_dict does not carry
            # filepath: still read what yt-dlp itself reports, not a guess built from the title.
            requested = info.get("requested_downloads") or []
            if requested and requested[0].get("filepath"):
                final_path[0] = requested[0]["filepath"]

        if final_path[0] is None:
            raise engines.EngineError("download succeeded but the extracted wav path could not be determined")

        elapsed = time.time() - started
        title = info.get("title")
        duration = info.get("duration")
        if report:
            report("done", f"downloaded {title or url!r} in {elapsed:.2f}s")

        return {
            "path": final_path[0],
            "title": title,
            "duration_s": float(duration) if duration is not None else None,
            "elapsed_s": elapsed,
        }


def _write(obj: dict) -> None:
    _PROTOCOL_OUT.write(json.dumps(obj) + "\n")
    _PROTOCOL_OUT.flush()


def _report_for(sidecar: Sidecar, req_id):
    """Builds the report(slug, text, fraction=None) closure passed into Sidecar.transcribe/
    download for one request, so the engine/download code never has to know req_id itself --
    it just calls report(), and this is what turns that into a stage event on the wire."""
    return lambda slug, text, fraction=None: sidecar.stage(req_id, slug, text, fraction)


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

        if cmd == "transcribe":
            wav = request.get("wav")
            if not wav:
                _write({"id": req_id, "ok": False, "error": "missing required field: wav"})
                continue

            engine = request.get("engine", "auto")

            try:
                result = sidecar.transcribe(wav, engine, report=_report_for(sidecar, req_id))
                _write({"id": req_id, "ok": True, **result})
            except Exception as exc:  # noqa: BLE001 - any engine/IO failure becomes ok:false, not a crash
                print(f"[quarry_sidecar] transcribe failed for id={req_id!r}: {exc}", file=sys.stderr)
                _write({"id": req_id, "ok": False, "error": str(exc)})
            continue

        if cmd == "download":
            url = request.get("url")
            out_dir = request.get("out_dir")
            if not url:
                _write({"id": req_id, "ok": False, "error": "missing required field: url"})
                continue
            if not out_dir:
                _write({"id": req_id, "ok": False, "error": "missing required field: out_dir"})
                continue

            try:
                result = sidecar.download(url, out_dir, report=_report_for(sidecar, req_id))
                _write({"id": req_id, "ok": True, **result})
            except Exception as exc:  # noqa: BLE001 - any download/ffmpeg failure becomes ok:false
                print(f"[quarry_sidecar] download failed for id={req_id!r}: {exc}", file=sys.stderr)
                _write({"id": req_id, "ok": False, "error": str(exc)})
            continue

        _write({"id": req_id, "ok": False, "error": f"unknown cmd: {cmd!r}"})

    # Falls through to here either from an explicit "shutdown" or from EOF on stdin ending the
    # for-loop naturally; both exit cleanly.
    return 0


def transcribe_once(wav: pathlib.Path, engine: str, json_out) -> int:
    sidecar = Sidecar()

    # Not part of the wire protocol here (see the module docstring), so stage events have nowhere
    # to go but stderr -- printed as plain text rather than JSON, same as everything else this
    # mode logs. fraction, when known, is appended as a percentage rather than dropped.
    def report(_slug, text, fraction=None):
        if fraction is not None:
            print(f"{text} ({fraction:.0%})", file=sys.stderr)
        else:
            print(text, file=sys.stderr)

    try:
        result = sidecar.transcribe(str(wav), engine, report=report)
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
