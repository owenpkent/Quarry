#!/usr/bin/env python3
"""Sidecar tests. Plain script, no pytest: run directly with
tools/bakeoff/.venv/Scripts/python.exe tools/sidecar/test_sidecar.py

(a) One-shot `quarry_sidecar.py transcribe` (a real subprocess, --json-out to a temp file) for
    the maestro corpus test wav, once per installed engine: response schema validity, notes
    sorted by onset, note count within 20% of an earlier tools/bakeoff run's count for this exact
    file (tools/bakeoff/out/maestro_real/<engine>/maestro_low1_2008_c4ab.mid), and a non-empty
    pedal list for kong on that pedalled corpus.
(b) `serve` mode driven as a real subprocess over its stdin/stdout pipe: the ready line, two
    transcribe requests (kong, transkun), an intentionally bad request (missing "wav") that must
    come back ok:false without killing the process, then a clean shutdown.
"""

import json
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

    proc = subprocess.Popen(
        [str(PYTHON), str(SIDECAR), "serve"],
        cwd=str(SCRIPT_DIR), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1,
    )

    stdout_q: "queue.Queue" = queue.Queue()
    stderr_lines = []

    def pump_stdout():
        for line in proc.stdout:
            stdout_q.put(line)
        stdout_q.put(None)

    def pump_stderr():
        for line in proc.stderr:
            stderr_lines.append(line)

    threading.Thread(target=pump_stdout, daemon=True).start()
    threading.Thread(target=pump_stderr, daemon=True).start()

    def send(obj):
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()

    def recv(timeout=240):
        try:
            line = stdout_q.get(timeout=timeout)
        except queue.Empty:
            check(False, f"got a response line within {timeout}s")
            return None
        if line is None:
            check(False, "stdout closed (EOF) before a response arrived")
            return None
        return json.loads(line)

    try:
        ready = recv(timeout=60)
        check(ready is not None and ready.get("event") == "ready", f"ready event: {ready!r}")
        if ready:
            check(ready.get("protocol") == 1, f"protocol version 1: {ready.get('protocol')!r}")
            check(isinstance(ready.get("engines"), list) and ready["engines"], f"engines list non-empty: {ready.get('engines')!r}")
            check(ready.get("device") in ("cuda", "cpu"), f"device is cuda or cpu: {ready.get('device')!r}")

        print(f"-- transcribe request 1 ({engine_a})...")
        send({"id": "req-1", "cmd": "transcribe", "wav": str(wav), "engine": engine_a})
        resp1 = recv()
        check(resp1 is not None and resp1.get("id") == "req-1", f"req-1 id echoed: {resp1.get('id') if resp1 else None!r}")
        if resp1 is not None:
            validate_schema(resp1)

        print(f"-- transcribe request 2 ({engine_b})...")
        send({"id": "req-2", "cmd": "transcribe", "wav": str(wav), "engine": engine_b})
        resp2 = recv()
        check(resp2 is not None and resp2.get("id") == "req-2", f"req-2 id echoed: {resp2.get('id') if resp2 else None!r}")
        if resp2 is not None:
            validate_schema(resp2)

        print("-- intentionally bad request (missing wav)...")
        send({"id": "req-bad", "cmd": "transcribe", "engine": engine_a})
        resp_bad = recv()
        check(resp_bad is not None and resp_bad.get("id") == "req-bad", f"bad request id echoed: {resp_bad!r}")
        if resp_bad is not None:
            check(resp_bad.get("ok") is False, f"bad request returns ok:false: {resp_bad!r}")
            check(isinstance(resp_bad.get("error"), str) and resp_bad["error"], "bad request carries an error message")

        check(proc.poll() is None, "process still alive after the bad request")

        print("-- shutdown...")
        send({"cmd": "shutdown"})
        try:
            exit_code = proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            exit_code = None
        check(exit_code == 0, f"clean shutdown, exit code {exit_code!r}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)
        print(f"(serve stderr: {len(stderr_lines)} line(s), suppressed)")


def main():
    test_one_shot()
    test_serve()

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
