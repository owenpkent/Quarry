# Quarry — The Bench

Companion to `ANALYSIS.md` §4.0, which argues for measuring before deciding and records what
was measured. This document is the operating manual for the measurement apparatus: the
corpora, the bench binary, the external-engine tooling, and the key bench. The numbers
themselves live in `ANALYSIS.md`; nothing here restates them.

The one command:

```
cmake -S . -B build -DQUARRY_BUILD_BENCH=ON
cmake --build build --target Bench --config Release
build\tools\bench\Bench_artefacts\Release\Bench.exe Tests\bench_corpus --baseline tools\bench\baseline.tsv
```

---

## 1. The corpora

A corpus is a directory of paired `<name>.wav` + `<name>.mid`: 30 second excerpts with exact
note-level ground truth (velocities preserved, offsets pedal-extended, and since 2026-08-18
the window's CC64 stream, including the pedal state at window open). All of them are
gitignored and regenerate deterministically (`seed 20260818`), because their sources are
non-commercial datasets or copyrighted arrangements that must never enter the repo; the
generators and their pinned URLs plus SHA-256 hashes are what is committed. Downloads cache
under `~/.okstudio/`, one folder per dataset, outside every build tree.

| corpus | material | source | generator |
| --- | --- | --- | --- |
| `Tests/bench_corpus` | synthetic additive piano, 7 cases | none needed | `tools/bench/make_corpus.py` |
| `Tests/bench_corpus_maestro` | real performances, sampled piano render, pedal-stratified (6 low / 8 mid / 8 high) | MAESTRO v3 MIDI + FluidSynth + Salamander Grand | `fetch_maestro.py`, then `make_real_corpus.py` |
| `Tests/bench_corpus_maestro_real` | the same 22 windows, real recordings | MAESTRO v3 audio (the 101 GB zip; extracted selectively) | `make_real_audio_corpus.py` (verifies its ground truth byte-identical to the rendered twin) |
| `Tests/bench_corpus_smd` | real Disklavier recordings, different piano and room | SMD via Zenodo | `make_smd_corpus.py` (alignment spot-checked, within 10 ms) |
| `Tests/bench_corpus_slakh` | multi-instrument mixes, per-stem ground truth | BabySlakh (CC BY, 16 kHz mono; per-stem audio kept in cache for separation work) | `make_slakh_corpus.py` |
| `Tests/bench_corpus_gm` | the local GM MIDI collection, stratified piano_solo / few_inst / full_mix, drums rendered but excluded from ground truth | `C:\Users\owenp\Ableton\MIDI` + FluidR3 GM | `make_gm_corpus.py` |

The twin pair matters most: rendered and real MAESTRO share windows and ground truth exactly,
which is what isolated the rendering bias (`ANALYSIS.md` §4.0: the CPU tier scores better on
its render, MAESTRO-trained models score better on the recording, so rendered corpora
understate the gap).

Generator venv: `tools/bench/.venv` (mido, pretty_midi, numpy, soundfile).

## 2. The bench binary

`tools/bench/Bench.cpp`, built with `-DQUARRY_BUILD_BENCH=ON`, deliberately compiled against
the shipping timestamp path. Columns: note-level P / R / F1 at the standard 50 ms onset
tolerance; `+off` requiring offsets within max(50 ms, 20% of note length); `+vel` requiring
velocity within 0.1 of the take's peak after an optimal global rescale; `pedal`, span-level
F1 on CC64 down-spans at the 200 ms tolerance Kong et al. use (`-` means the reference has no
pedal or the engine cannot produce any, `0.000` means it could and found none); mean absolute
onset error in ms. Matching is maximum bipartite, as `mir_eval` does it; aggregation sums
counts across cases rather than averaging F1.

Flags:

- `--legacy` runs the pre-§2-fix engine, so a change is attributable.
- `--baseline <tsv>` exits non-zero when aggregate onset F1 falls; `--write-baseline` records.
- `--dump-notes <dir>` writes `<case>.est.tsv` (onset, offset, pitch, velocity 0-127) and,
  when the engine produced pedal, `<case>.pedal.tsv` (time, value). This is also how other
  tools consume the engine headlessly.
- `--sidecar "<command>"` transcribes through the out-of-process sidecar instead of Basic
  Pitch (`--engine` picks which model; see `SIDECAR.md`), everything else identical.

**The arithmetic is validated, not trusted**: `tools/bakeoff/crosscheck_mir_eval.py` scores a
corpus plus a `--dump-notes` directory with `mir_eval.transcription` and matched the bench
exactly, per case and aggregate, on onsets. Known residual: the offset column differs
slightly (0.246 vs 0.202 aggregate on one corpus), unresolved, so treat `+off` as internally
consistent rather than literature-comparable until someone chases it.

## 3. External engines

`tools/bakeoff/run_bakeoff.py` (venv `tools/bakeoff/.venv`, setup in `SIDECAR.md` §1) runs
kong / transkun / muscriptor / the bench's own dump over a corpus and scores every engine
with the same `mir_eval` code, so a candidate's claims land on the same yardstick:

```
tools\bakeoff\.venv\Scripts\python tools\bakeoff\run_bakeoff.py Tests\bench_corpus_maestro_real --engines dump,kong,transkun --dump-dir <dump>
```

Since the sidecar exists, `Bench.exe --sidecar --engine <name>` measures the same engines
through Quarry's own pipeline, which is the number that counts; the Python runner remains
useful for engines not yet wired into the sidecar.

## 4. The key bench

`tools/keybench/`: `fetch_giantsteps.py` pulls the GiantSteps Key dataset (604 key-labelled
Beatport excerpts; the working audio mirror is `www.cp.jku.at/datasets/giantsteps/backup/`,
MD5-verified against the dataset's own hashes; `giantsteps_labels.tsv` is committed so the
harness works if upstream vanishes). `score_keys.py` rebuilds `KeyEstimate`'s exact
duration-times-velocity histogram from `--dump-notes` output or MIDI and scores four profile
sets (Krumhansl-Kessler with `KeyEstimate.cpp`'s constants, Temperley-Kostka-Payne,
Albrecht-Shanahan, a doubled-tonic modal set) by accuracy and the MIREX weighted score. Its
`--self-test` recovers 24/24 keys per profile and proves numerically that relative major and
minor are indistinguishable to a flat diatonic template (`STATS.md` §4.2 item 4, now with a
proof). **Status: harness complete, measurement not yet run** (19 of 604 audio files cached);
`STATS.md` §4.3 gates all key rework on it.

## 5. Reading the results

Every measured number, with the argument it settled, is recorded at the end of
`ANALYSIS.md` §4.0 (the corpus ladder and what real material did to the synthetic numbers)
and §4.2 (the four-engine verdict, the sidecar's own end-to-end numbers, pedal, separation).
The bench exists so that no claim in those sections is older than the command that reproduces
it.
