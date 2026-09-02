#!/usr/bin/env python3
"""Sidecar tests. Plain script, no pytest: run directly with
tools/bakeoff/.venv/Scripts/python.exe tools/sidecar/test_sidecar.py

(a) One-shot `quarry_sidecar.py transcribe` (a real subprocess, --json-out to a temp file) for
    the maestro corpus test wav, once per installed engine: response schema validity, notes
    sorted by onset, note count within 20% of an earlier tools/bakeoff run's count for this exact
    file (tools/bakeoff/out/maestro_real/<engine>/maestro_low1_2008_c4ab.mid), and a non-empty
    pedal list for kong on that pedalled corpus.
(b) `serve` mode driven as a real subprocess over its stdin/stdout pipe: the ready line, two
    transcribe requests (kong, transkun) -- checking that "stage" events arrive before each
    response, carry that request's id, and include the `infer` stage the engine reports from
    inside transcribe()'s stdout redirect -- an intentionally bad request (missing
    "wav") that must come back ok:false without killing the process, then a clean shutdown.
(c) `download`, over the same `serve` subprocess: only runs when QUARRY_TEST_DOWNLOAD_URL is set
    in the environment (skipped with a printed note otherwise, since it needs network access and
    a real URL); checks a wav lands on disk, stage events arrive, and the response schema.
"""

import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
PYTHON = SCRIPT_DIR.parent / "bakeoff" / ".venv" / "Scripts" / "python.exe"
SIDECAR = SCRIPT_DIR / "quarry_sidecar.py"

sys.path.insert(0, str(SCRIPT_DIR))
import engines  # noqa: E402

FAILURES = []


def check(condition, message):
    if condition:
        print(f"  ok: {message}")
    else:
        print(f"  FAIL: {message}")
        FAILURES.append(message)


def find_wav():
    preferred = REPO_ROOT / "Tests" / "bench_corpus_maestro_real" / "maestro_low1_2008_c4ab.wav"
    if preferred.is_file():
        return preferred, True

    fallback_dir = REPO_ROOT / "Tests" / "bench_corpus"
    for wav in sorted(fallback_dir.glob("*.wav")):
        return wav, False

    raise FileNotFoundError("no test wav found under Tests/bench_corpus_maestro_real or Tests/bench_corpus")


# Note counts from an earlier tools/bakeoff run on this exact file (counted directly from
# tools/bakeoff/out/maestro_real/<engine>/maestro_low1_2008_c4ab.mid with pretty_midi). Only
# meaningful for the preferred maestro_low1_2008_c4ab.wav case.
BASELINE_NOTE_COUNTS = {"kong": 159, "transkun": 159, "muscriptor": 152}


def validate_schema(response):
    """Checks common to every ok:true response. Returns (notes, pedal)."""
    check(response.get("ok") is True, f"ok:true (error={response.get('error')!r})")
    check(response.get("engine") in ("kong", "transkun", "muscriptor"),
          f"engine field is a real engine name: {response.get('engine')!r}")
    elapsed = response.get("elapsed_s")
    check(isinstance(elapsed, (int, float)) and elapsed >= 0, f"elapsed_s is a non-negative number: {elapsed!r}")

    notes = response.get("notes")
    check(isinstance(notes, list), "notes is a list")
    notes = notes or []
    onsets = [n["onset"] for n in notes]
    check(onsets == sorted(onsets), "notes sorted by onset ascending")

    sample = notes if len(notes) <= 10 else notes[:5] + notes[-5:]
    for n in sample:
        check(isinstance(n.get("pitch"), int), f"note pitch is int: {n.get('pitch')!r}")
        check(n.get("velocity") is None or isinstance(n.get("velocity"), int),
              f"note velocity is int or null: {n.get('velocity')!r}")
        check(isinstance(n.get("instrument"), str) and n["instrument"], f"note instrument is a non-empty str")

    pedal = response.get("pedal")
    check(isinstance(pedal, list), "pedal is a list")
    pedal = pedal or []
    times = [p["time"] for p in pedal]
    check(times == sorted(times), "pedal sorted by time ascending")
    for p in pedal[:5]:
        check(isinstance(p.get("value"), int) and 0 <= p["value"] <= 127, f"pedal value is int 0-127: {p.get('value')!r}")

    check(isinstance(response.get("warnings"), list), "warnings is a list")

    return notes, pedal


def test_one_shot():
    print("\n=== (a) one-shot transcribe ===")
    wav, is_preferred = find_wav()
    print(f"test wav: {wav} (preferred maestro case: {is_preferred})")

    available = sorted(engines.available_engines())
    check(bool(available), f"at least one engine available: {available}")

    with tempfile.TemporaryDirectory(prefix="quarry_sidecar_test_") as tmp:
        for engine in ["kong", "transkun", "muscriptor"]:
            if engine not in available:
                print(f"-- {engine}: skip (not installed)")
                continue

            out_path = pathlib.Path(tmp) / f"one_shot_{engine}.json"
            print(f"-- {engine}: running one-shot...")
            started = time.time()
            proc = subprocess.run(
                [str(PYTHON), str(SIDECAR), "transcribe", str(wav), "--engine", engine, "--json-out", str(out_path)],
                cwd=str(SCRIPT_DIR), capture_output=True, text=True, timeout=600,
            )
            wall_elapsed = time.time() - started

            check(proc.returncode == 0, f"{engine}: exit code 0 (got {proc.returncode}; stderr tail: {proc.stderr[-800:]!r})")
            if proc.returncode != 0 or not out_path.is_file():
                continue

            response = json.loads(out_path.read_text(encoding="utf-8"))
            notes, pedal = validate_schema(response)

            print(f"   {engine}: {len(notes)} notes, {len(pedal)} pedal events, "
                  f"elapsed_s={response.get('elapsed_s'):.2f} (wall {wall_elapsed:.1f}s)")

            if is_preferred and engine in BASELINE_NOTE_COUNTS:
                baseline = BASELINE_NOTE_COUNTS[engine]
                low, high = baseline * 0.8, baseline * 1.2
                check(low <= len(notes) <= high,
                      f"{engine}: note count {len(notes)} within 20% of bake-off baseline {baseline} "
                      f"({low:.0f}-{high:.0f})")

            if is_preferred and engine == "kong":
                check(len(pedal) > 0, "kong: pedal non-empty on the pedalled maestro corpus")


class _ServeHarness:
    """A `serve`-mode subprocess plus the send/recv plumbing every test that drives one needs.
    Factored out of test_serve() so test_download() (a second serve-mode test) does not have to
    duplicate the pump threads and the stage-event-aware recv loop."""

    def __init__(self):
        self.proc = subprocess.Popen(
            [str(PYTHON), str(SIDECAR), "serve"],
            cwd=str(SCRIPT_DIR), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self.stdout_q: "queue.Queue" = queue.Queue()
        self.stderr_lines = []

        threading.Thread(target=self._pump_stdout, daemon=True).start()
        threading.Thread(target=self._pump_stderr, daemon=True).start()

    def _pump_stdout(self):
        for line in self.proc.stdout:
            self.stdout_q.put(line)
        self.stdout_q.put(None)

    def _pump_stderr(self):
        for line in self.proc.stderr:
            self.stderr_lines.append(line)

    def send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def recv_line(self, timeout=240):
        """One parsed JSON line, whatever it is (a response, or an event like "ready" or
        "stage")."""
        try:
            line = self.stdout_q.get(timeout=timeout)
        except queue.Empty:
            check(False, f"got a response line within {timeout}s")
            return None
        if line is None:
            check(False, "stdout closed (EOF) before a response arrived")
            return None
        return json.loads(line)

    def recv_response(self, timeout=240):
        """Reads lines until the request's response arrives (an "ok" line, not an "event" line),
        collecting any "stage" events seen along the way. Returns (response, stage_events)."""
        stages = []
        while True:
            obj = self.recv_line(timeout=timeout)
            if obj is None:
                return None, stages
            if obj.get("event") == "stage":
                stages.append(obj)
                continue
            return obj, stages

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)


def test_serve():
    print("\n=== (b) serve mode ===")
    wav, _ = find_wav()
    available = engines.available_engines()

    engine_a = "kong" if "kong" in available else next(iter(available), None)
    engine_b = "transkun" if "transkun" in available and "transkun" != engine_a else \
        next((e for e in available if e != engine_a), engine_a)

    check(engine_a is not None, "at least one engine available for serve test")
    if engine_a is None:
        return

    harness = _ServeHarness()

    try:
        ready = harness.recv_line(timeout=60)
        check(ready is not None and ready.get("event") == "ready", f"ready event: {ready!r}")
        if ready:
            check(ready.get("protocol") == 2, f"protocol version 2: {ready.get('protocol')!r}")
            check(isinstance(ready.get("engines"), list) and ready["engines"], f"engines list non-empty: {ready.get('engines')!r}")
            check(ready.get("device") in ("cuda", "cpu"), f"device is cuda or cpu: {ready.get('device')!r}")

        print(f"-- transcribe request 1 ({engine_a})...")
        harness.send({"id": "req-1", "cmd": "transcribe", "wav": str(wav), "engine": engine_a})
        resp1, stages1 = harness.recv_response()
        check(resp1 is not None and resp1.get("id") == "req-1", f"req-1 id echoed: {resp1.get('id') if resp1 else None!r}")
        check(len(stages1) > 0, "at least one stage event arrived before the transcribe response")
        if stages1:
            check(all(s.get("id") == "req-1" for s in stages1), "stage events carry the request's id")

            # Named explicitly, not just counted. transcribe() runs the engine under
            # contextlib.redirect_stdout(sys.stderr) to keep library chatter off the wire, and
            # that redirect used to swallow every stage the engine itself reported -- `infer`
            # above all -- while `received`/`load-model`/`post`/`done` still arrived from outside
            # the block. A "len(stages) > 0" check passes happily through that bug; the caller's
            # progress readout does not, since `infer` is the long part of the request.
            slugs = {s.get("stage") for s in stages1}
            check("infer" in slugs, f"the infer stage reached the caller, not just stderr: {sorted(slugs)}")
            check("done" in slugs, f"the done stage reached the caller: {sorted(slugs)}")
        if resp1 is not None:
            validate_schema(resp1)

        print(f"-- transcribe request 2 ({engine_b})...")
        harness.send({"id": "req-2", "cmd": "transcribe", "wav": str(wav), "engine": engine_b})
        resp2, _stages2 = harness.recv_response()
        check(resp2 is not None and resp2.get("id") == "req-2", f"req-2 id echoed: {resp2.get('id') if resp2 else None!r}")
        if resp2 is not None:
            validate_schema(resp2)

        print("-- intentionally bad request (missing wav)...")
        harness.send({"id": "req-bad", "cmd": "transcribe", "engine": engine_a})
        resp_bad, _stages_bad = harness.recv_response()
        check(resp_bad is not None and resp_bad.get("id") == "req-bad", f"bad request id echoed: {resp_bad!r}")
        if resp_bad is not None:
            check(resp_bad.get("ok") is False, f"bad request returns ok:false: {resp_bad!r}")
            check(isinstance(resp_bad.get("error"), str) and resp_bad["error"], "bad request carries an error message")

        check(harness.proc.poll() is None, "process still alive after the bad request")

        print("-- shutdown...")
        harness.send({"cmd": "shutdown"})
        try:
            exit_code = harness.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            harness.proc.kill()
            exit_code = None
        check(exit_code == 0, f"clean shutdown, exit code {exit_code!r}")
    finally:
        harness.close()
        print(f"(serve stderr: {len(harness.stderr_lines)} line(s), suppressed)")


def test_download():
    print("\n=== (c) download ===")
    url = os.environ.get("QUARRY_TEST_DOWNLOAD_URL")
    if not url:
        print("-- skip: QUARRY_TEST_DOWNLOAD_URL is not set")
        return

    with tempfile.TemporaryDirectory(prefix="quarry_sidecar_download_test_") as tmp:
        harness = _ServeHarness()

        try:
            ready = harness.recv_line(timeout=60)
            check(ready is not None and ready.get("event") == "ready", f"ready event: {ready!r}")

            print(f"-- download request ({url})...")
            harness.send({"id": "dl-1", "cmd": "download", "url": url, "out_dir": tmp})
            resp, stages = harness.recv_response(timeout=300)

            check(resp is not None and resp.get("id") == "dl-1", f"dl-1 id echoed: {resp.get('id') if resp else None!r}")
            check(len(stages) > 0, "at least one stage event arrived before the download response")
            if stages:
                check(all(s.get("id") == "dl-1" for s in stages), "download stage events carry the request's id")

            if resp is not None:
                check(resp.get("ok") is True, f"download ok:true (error={resp.get('error')!r})")
                if resp.get("ok"):
                    path = resp.get("path")
                    check(isinstance(path, str) and pathlib.Path(path).is_file(), f"downloaded wav exists on disk: {path!r}")
                    check(isinstance(resp.get("title"), str) and resp["title"], f"title is a non-empty str: {resp.get('title')!r}")
                    duration = resp.get("duration_s")
                    check(duration is None or isinstance(duration, (int, float)), f"duration_s is a number or null: {duration!r}")
                    elapsed = resp.get("elapsed_s")
                    check(isinstance(elapsed, (int, float)) and elapsed >= 0, f"elapsed_s is a non-negative number: {elapsed!r}")

            print("-- shutdown...")
            harness.send({"cmd": "shutdown"})
            try:
                harness.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                harness.proc.kill()
        finally:
            harness.close()


def main():
    test_one_shot()
    test_serve()
    test_download()

    print("\n=== summary ===")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1

    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
