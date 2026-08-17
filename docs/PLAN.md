# Quarry — Build Plan

> ## Status, 2026-08-17 — the analysis engine, and a reordering
>
> The milestone list below still describes the product build. This block records four
> decisions taken on the analysis engine that reorder the parts of it touching transcription,
> and the finding that forced the reorder. `ANALYSIS.md` §5 carries the resulting sequence and
> `STATS.md` covers the report layer, which is new.
>
> **Decided.**
> - **The GPU sidecar leads**, not the CPU fix list. What fails on real takes is missed and
>   invented notes, and dynamics. That is a model-quality complaint first.
> - **Personal tool now, door open later.** CC BY-NC weights and non-commercial datasets are
>   available for personal use and for the bench. Nothing about that is allowed to put them
>   into Quarry's binary or its distribution, which is what §4.2's out-of-process design
>   already buys, so this costs nothing to honour.
> - **All three outputs are wanted**: a `.mid` to edit, a starting project in the DAW, and a
>   readout to look at. The middle one makes tempo and sections load-bearing rather than
>   informational; the third is what `STATS.md` is for.
> - **Piano leads**, with the acoustic chain (`STATS.md` §6) as a second track, since it shares
>   no code with the model path and cannot be blocked by transcription quality.
>
> **The finding that reordered it.** MuScriptor encodes no velocity. A better model therefore
> fixes the first complaint and makes the second one permanent, because there is no amplitude
> anywhere in its output. So the velocity work in `ANALYSIS.md` §2.1 is required on every path
> and has been respecified as a tier-independent stage rather than a patch to `Notes.cpp`.
> Two further limits are in `ANALYSIS.md` §3.0.1; the one that matters here is that its
> tokenizer cannot represent two notes of the same pitch sounding at once, which is a pedalled
> piano re-strike, which is the primary use case.
>
> **Next.** Spike the sidecar before integrating any of it. Nothing else is decidable until it
> has run on real takes of both kinds.
>
> ---
>
> ## Status, 2026-07-31
>
> **The day-one spike in Milestone 0 has landed and is committed** on branch
> `audio-input-and-recording` in `c:/Users/owenp/dev/NeuralNoteVideo`. The fork's product has
> also been renamed to Quarry, so "Quarry" now names both the shipped fork and the planned
> new repo below; the checkout path and the git remote are still `NeuralNoteVideo`. See
> `## [Unreleased]` in `CHANGELOG.md` for what the spike does, and the "What landed" note in
> Milestone 0 below.
>
> **One correction to this plan.** All three reviewers independently ranked the spike, not
> the new-repo build, as the correct Milestone 1, and said so more strongly than the plan
> below reflects: get the whole loop into Owen's hands *before* any architecture is locked,
> because a week of him actually using it settles the capture-model question (Q1 in
> `MORNING.md`), the describe-scope question and the roll-posture question more cheaply and
> more correctly than any further planning can. Read the milestone numbering below as
> "what to build once he has used the spike and answered", not as a queue to start now.
>
> **Two things in this plan were written on assumptions that turned out to be wrong**, both
> found while building the spike, both now verified against the source:
> - Transcribe-on-stop did not need building. `SourceAudioManager::stopRecording()` already
>   calls `TranscriptionManager::setLaunchNewTranscription()`, and there is no Transcribe
>   button in the UI at all.
> - The failing `UnitTests` note-conversion test is **not** caused by the short-input
>   underflow guard in `6c7699e`, and is not a fork regression. `Lib/` and `Tests/` are
>   byte-identical to upstream apart from that guard and an LTO flag; the golden file
>   `Tests/test_data/note_events.output.json` was last regenerated at `1b4b46e`, and upstream
>   then changed note conversion twice after it (`9421055`, `0a339f2`). It fails on upstream
>   NeuralNote too. Any milestone here that claims per-note confidence still needs it green,
>   but the fix is regenerating stale golden data, not reverting a guard — and regenerating
>   it blindly just enshrines current behaviour, so it needs ground truth first.

Companion to `DESIGN.md`. Milestones are sequential unless marked otherwise.
Estimates are working days for one person and are deliberately not optimistic: two of the
estimates in the source proposals were off by 3x and one rested on an extraction nobody had
verified.

**Repos.** `c:/Users/owenp/dev/NeuralNoteVideo` is the donor fork and the vehicle for the
day-one spike. `c:/Users/owenp/dev/Quarry` is the product. `../JUCE` and
`../okstudio-juce-kit` are siblings.

*Superseded on the kit.* This plan said no `ThirdParty/` tree ever lands in Quarry. The fork
now **vendors** the two kit headers it needs at `ThirdParty/okstudio/include/okstudio/`,
pinned in `ThirdParty/okstudio/UPSTREAM.txt` and re-synced by `py tools/sync_okstudio.py`,
with a CMake warning when a local kit checkout has drifted from the vendored copy. The kit
repo is private, so a submodule or a required sibling checkout would leave a public clone
unable to build at all; a sibling checkout is now a convenience for syncing, never a build
dependency. Whether the new repo consumes the kit the same way is still open, and the answer
should follow the same reasoning rather than this line.

**Legend.** 🔴 BLOCKED ON OWEN — do not start. 🟡 GATED — start only if a measurement passes.

---

## Milestone 0 — Preflight, the day-one spike, and the rename

**Goal.** Owen has a working end-to-end loop tomorrow morning, the two existential risks
are retired, the kit is widened, and the Quarry repo exists.

Four parts, in this order. **Part A ships on day 1** and is deliberately throwaway.

### 0A. The spike, in the existing NeuralNoteVideo checkout (2 days)

Owen double-clicks `run.py`, plays a YouTube video, clicks **Record**, clicks **Stop**, and
drags the MIDI onto an Ableton track holding Simpler. That is the entire product thesis
working, in the fork, before a single architectural decision is locked — and a week of him
actually using it will settle more than any further planning can.

- Graft `okstudio/WasapiLoopback.h` in behind the existing
  `AudioInputManager` as a synthetic **"System Audio (loopback)"** entry, made the **default**
  selection in `AudioInputView`'s driver dropdown. The fork has zero loopback support (no
  matches for `loopback` or `WASAPI` in `AudioInputManager.{h,cpp}`); the kit already ships it
  (`AudioCapture.h:40` `loopbackTypeName`, `:142`). This is a graft, not a build.
- Route its blocks into `SourceAudioManager::processExternalInputBlock`.
- **Auto-run transcription on Stop.** No third click.
- Enlarge the Record button to 180 × 72.

*Not 1 day.* The kit header was assumed to need C++20 against the fork's C++17
(`CMakeLists.txt:5`), the fork has no kit dependency, and the loopback poll thread delivers at
the endpoint's rate into a `_writeBlock` that takes a `ScopedLock` and expects host-rate
blocks. Budget 2.

**Files:** `Quarry/Source/AudioInputManager.{h,cpp}`,
`Quarry/Source/Components/Views/AudioInputView.{h,cpp}`,
`Quarry/Source/SourceAudioManager.cpp`, `CMakeLists.txt` (add the kit include path).
**Done when:** Owen reports he captured a YouTube clip and heard it back through Simpler.
**Throw away after M1.**

**What landed**, and where it differs from the four bullets above:

- The driver entry is called **"System Audio"**, not "System Audio (loopback)". It is the
  default on Windows on first run only, pointed at the default playback endpoint, and the
  choice is then the user's; elsewhere the standalone starts on the machine's default input.
- The C++20 bump was not needed. `WasapiLoopback.h` compiles at C++17, so `CMakeLists.txt:5`
  is unchanged and the only CMake change is the include path plus the drift check.
- The kit headers are **vendored**, not consumed from the sibling checkout. See the note under
  *Repos* above.
- Transcribe-on-stop needed no work, as the status block at the top says.
- Beyond the four bullets, and not planned here: picking a device is standalone-only (a hosted
  plugin never constructs an `AudioDeviceManager`, because an ASIO driver serves one client at
  a time), loopback endpoints are remembered by their stable Windows endpoint id rather than by
  name, a capture that dies mid-take reports an error and ends the take instead of silently
  truncating it, and `UpdateCheck` was pointed at this fork's own origin.
- Superseded: the audio input panel had a 180 × 72 Record button of its own. The panel is now
  the docked SOURCE strip and that button is gone, since it only ever routed back to the
  toolbar's. Recording has one way in and out, at 35 × 35 in the toolbar.

### 0B. Retire the two existential risks (1 day)

1. **Mirror the ONNX Runtime tarball.** It is a **~100 MB** download (`ONNX_URL` in `run.py`), not
   2.9 GB — the inflated number has been getting this deferred. It comes from one individual's
   personal GitHub release published once in March 2023
   (`tiborvass/libonnxruntime-neuralnote v1.14.1-neuralnote.1`) and is gitignored. Copy it to
   storage Owen controls; point `OKSTUDIO_ONNXRUNTIME_CACHE` at a stable directory outside
   every build tree; document it in the kit.
2. **Triage the red notes test.** `UnitTests.exe` emits **16** note events against a 9-event
   golden file (feature and CNN stages pass, max error 8.3e-04 / 6.9e-07). The got-list
   contains the expected events plus extras, so the note stage emits a superset. Check out
   `6c7699e^`, rebuild the test target only, rerun. Either the golden file drifted from the
   current defaults or the underflow-guard commit changed behaviour.
   **Every confidence number Quarry will display comes out of that stage. Nothing downstream
   ships until it is green or the drift is explained in writing.**

**Done when:** the tarball resolves from Owen's mirror on a clean tree, and the notes test is
green or a written explanation of the delta exists in `docs/`.

### 0C. Widen the kit (3 days)

Do this **before** Quarry consumes it; every product on the line benefits.

1. **`okstudio/Transcribe.h`** — add
   `struct Posteriorgrams { std::vector<std::vector<float>> notes, onsets, contours; double frameRate; }`
   and `const Posteriorgrams& posteriorgrams() const`. The pimpl currently returns only
   `vector<Note>` and throws them away, and they are the entire description pillar.
2. **Split `Note::amplitude` into three.** `confidence` (mean note-PG, what it already is),
   `onsetConfidence` (peak onset-PG at the onset frame), `velocity` (RMS of the source audio
   over the note's span). Today one number is written into MIDI velocity
   (`MidiFileWriter.cpp:35`) *and* the preview synth (`SynthController.cpp:42`), so exported
   dynamics are the model's uncertainty. This is a latent bug fix, not a feature.
3. **`okstudio/AudioCapture.h`** — split `listen(const Source&)` out of `start()`. Verified
   coupling: `:205` `start()` opens the device and `:263` immediately calls `recorder.start()`,
   so there is currently no way to hear or meter a source without writing a WAV. Add
   `isListening()`, `struct BlockSink { virtual void captureBlock(const float* const*, int, int, double) noexcept = 0; }`
   and `setSink(BlockSink*)` handed over with `TakeRecorder`'s atomic-pointer + in-use-flag
   discipline. `handleBlock()` (`:448`) calls the sink before the recorder.
4. **Move `MidiFileWriter` into the kit** beside `Transcribe.h` and have `toMidiFile()`
   delegate to it. The kit's current `toMidiFile(notes, bpm)` drops the tempo map and the
   per-note pitch bend that the fork's writer already bakes at 960 TPQN.
5. **`okstudio/RingBuffer.h`** — new, pure, `juce_core` only. Lock-free SPSC ring of
   interleaved float frames, **each block stamped with its QPC timestamp**, wait-free write,
   and `reserve(seconds)` / `release()` so the message thread can claim a region against the
   writer without copying on the click path.

**Tests (kit test exe, links `okstudio_kit` alone):** posteriorgram dimensions match
`frames × {264, 88, 88}`; `velocity != confidence` on a synthetic buffer where they must
differ; `RingBuffer` reserve/release against a synthetic writer that laps the region; **a
deliberate gap injected into the writer is zero-padded to real time, not closed up.**
**Done when:** those tests pass and Keys still builds against the widened kit.

### 0D. Create the Quarry repo (2 days)

- `okstudio_add_plugin(Quarry PRODUCT_NAME "Quarry" PLUGIN_CODE Quar BUNDLE_SUFFIX quarry)`
  → `FORMATS VST3 Standalone`, manufacturer `OKSt`, `NEEDS_MIDI_INPUT TRUE`
  (non-negotiable — Live silently refuses an instrument without it and pluginval does not
  catch it), `EDITOR_WANTS_KEYBOARD_FOCUS FALSE`. C++20.
- **`run.py` is a copy of Strata's**, with the two Quarry-specific grafts. Strata's has:
  1200 s Smart App Control deadline (not 240), `launch_detached()` on a worker thread with a
  live counter and an honoured Ctrl+C, `--hold` plus the `run.ps1` shim,
  `KeyboardInterrupt` **and** `SystemExit` caught around `main()` so a file dropped on it in
  Explorer does not close on the argparse usage message, an LNK1104 warning in
  `close_running`, exact window-title match on `"Quarry"`. Graft on: `ensure_submodules()`,
  `ensure_onnxruntime()` (pointed at the mirror), and the forced `-DLTO=OFF` reconfigure —
  the prebuilt lib is `/GL` and linking fails `C1047` otherwise, which is exactly why
  `build.bat` cannot build the fork.
- **`src/QuarryParams.h` and `src/QuarryId.h`** — the one-time rename, `versionHint 1`.
  `OwKe`→`OKSt`, `NNVd`→`Quar`, every parameter id and ValueTree key. Append-only forever
  after this.
- **Apache-2.0 §4(b), satisfied on day one.** `LICENSE`, `NOTICE` (Damien Ronssin, Tibor
  Vass, Spotify basic-pitch — and check whether `spotify/basic-pitch` ships its own NOTICE
  whose contents must be reproduced), `// Modified 2026 by Owen Kent for Quarry.` above every
  retained `Created by Damien Ronssin` header with the original **retained** (stripping it
  breaches §4(c)). `docs/MODEL_LICENSES.md`, `docs/ABLETON.md`, `CHANGELOG.md`, `CLAUDE.md`.
- Copy `Keys/scripts/capture-window.ps1` unchanged apart from the exe path.
- **Ported from the fork, four things and nothing else:** `MidiFileDrag`,
  `TimeQuantizeOptions` + `TimeQuantizeUtils.h` (as a pure header), `SynthController` (the
  16-voice MPESynthesiser shrinks to one audition voice), and `Playhead::computePlayheadPositionPixel`
  (already a static pure function). `MidiFileWriter` went into the kit in 0C.
- **Deleted outright, immediately:** `UpdateCheck`. It polls a public GitHub releases API on
  every launch, which is the wrong update story for a sold product. *(It pointed at
  `DamRsn/NeuralNote/releases/latest`, sending customers to the free upstream. That is fixed
  in the fork, which now polls `owenpkent/NeuralNoteVideo`, but the reason to drop it here
  stands.)*

**Done when:** `py run.py` builds and launches an empty-shell Quarry standalone titled
"Quarry", and `Tests/` links `okstudio_kit` and runs green.

**M0 total: 8 days.** 🔴 *0D should not start until Owen has answered the ASIO and JUCE
questions — both change what `okstudio_add_plugin` is allowed to emit.*

---

## Milestone 1 — The KEEP loop *(usable by double-clicking `run.py`)*

**Goal.** Quarry listens from launch. Owen plays a YouTube video, clicks **KEEP**, and thirty
seconds of what he just heard becomes a take: transcribed, shown in a read-only roll, and
drag-ready from the header MIDI plate. **Two gestures from hearing it to owning it in
Ableton.**

**Files created:** `src/listen/Listener.{h,cpp}` (implements `okstudio::capture::BlockSink`,
owns a 300 s stereo ring at the endpoint rate), `src/TakeStore.{h,cpp}`,
`src/AnalysisJob.{h,cpp}` (one-thread `juce::ThreadPool`, `shouldExit()` between stages,
publishes per-stage via `MessageManager::callAsync`), `src/ui/HeaderStrip.{h,cpp}`,
`src/ui/SourceSection.{h,cpp}`, `src/ui/SectionBar.h` / `DetachedWindow.h` /
`StepComboBox.h` (copied verbatim from Keys, namespace `keys` → `quarry`),
`src/PluginProcessor.{h,cpp}`, `src/PluginEditor.{h,cpp}` with the four-entry `Section` table.

**Deliberate constraints in this milestone:** no live musical readout (level meter and
elapsed time only), no editing, no descriptions, no focus filter. The roll is the fork's
read-only `PianoRoll` transplanted, and **it is explicitly a placeholder** — its entire mouse
handling is one line that moves the playhead (`PianoRoll.cpp:125-128`).

**Tests:** `RingBuffer` gap-padding and reserve/release (from 0C, now exercised against the
real capture thread); `TakeStore` round-trips a `take.quarrytake` ValueTree; a take sealed,
Quarry closed, Quarry reopened, take restored with notes intact; an analysis job cancelled
mid-stage leaves no partial take directory.

**Definition of done:** Owen double-clicks `run.py`, plays a video, clicks KEEP, sees notes
within ~2 s, drags the MIDI onto a Simpler track, closes Quarry, reopens it, and the take is
still there. The header KEEP plate cannot be hidden by any fold, detach or layout state.

**8 days.** *(Then throw away 0A.)*

---

## Milestone 2 — Tempo, and the end of the fictional grid

**Goal.** Exported MIDI carries a real tempo map. This is a **bug fix**, not a description
feature, and it is second in the plan for that reason: until it lands, every `.mid` Quarry
produces lands in Live at the wrong tempo — at exactly the moment the product is supposed to
deliver.

**Files:** `src/analysis/TempoEstimator.h` (pure, `juce_core` only — autocorrelate the summed
onset posteriorgram over lags 0.3–1.2 s, then phase-align to maximise onset energy on beats),
`src/analysis/AnalysisReport.h`, `TimeQuantizeOptions.cpp` (feed the estimate into
`TimeQuantizeInfo::bpm` as the **standalone** source; keep the playhead as the override when
hosted), the ANALYSIS section bar with permanent `÷2` / `×2` at 44 × 24.

**Tests:** synthetic click trains at 60/90/120/174 BPM recover within ±1 %; a train at 120
does not recover as 60 or 240 more often than a stated tolerance; `×2` / `÷2` re-derive the
bar grid and the exported tempo map; `TimeQuantizeInfo::bpm` is the estimate in standalone and
the playhead when hosted.

**Definition of done:** a MIDI file exported from a 92 BPM capture opens in Live at 92 BPM
with bar 1 on beat 1, and a wrong answer is one click from right.

**4 days.**

---

## Milestone 3 — Confidence, honestly

**Goal.** Owen can see which notes the model is unsure about and jump straight to the bars
that need work.

**Prerequisite: 0B's red-test triage must be green.** Do not build a confidence UI on a stage
whose regression test emits a superset.

**Files:** `src/QuarryNotes.h` (the `Note` struct with the three split fields), the per-bar
confidence ribbon (8 px, non-interactive) and the *Next uncertain bar* button (190 × 34,
floating, always visible) in the roll, the `CONFIDENCE_FLOOR` rotary with its 34 px stepper
twin, note-body shading with a legend, and the confidence summary line in ANALYSIS.

**Tests:** `confidence` and `velocity` are provably different fields on a synthetic buffer;
per-bar 10th-percentile aggregation is correct at bar boundaries and on a partial final bar;
*Next uncertain bar* wraps and terminates rather than sticking on the last bar; notes below the
floor render at 25 % alpha and are still hit-testable.

**Definition of done:** on a real YouTube capture, Owen can click *Next uncertain bar*
repeatedly and each click lands on a bar he agrees is wrong.

**4 days.**

---

## Milestone 4 — The editable roll

**Goal.** Owen can hear a bad note, click it, and fix it.

**This is the largest single risk in the plan and it starts with a spike.**
`Lattice/src/ui/RollCanvas.h:32` and `:76` — the constructor takes `LatticeProcessor&` and the
class holds a reference, with **15 distinct processor methods over 37 call sites, every one
step-grid shaped** (`numSteps`, `stepsPerBar`, `velAt(row, step)`,
`noteStartCovering(row, step)`). The geometry and the gesture vocabulary port; the model, the
paint code and the hit testing are all written against a cell grid. **Spend one afternoon
compiling `RollCanvas` against a stub `QuarryNotes` model before committing the milestone.**
If it does not separate, this is a from-scratch build and the estimate roughly doubles.

**Reused verbatim from Lattice:** the `Geom`/`rowAt`/`rowTop`/`clampScroll` math, `rowH` as a
**constant 36.0f** with both `static_assert`s against `okstudio::ui::minHitPx`, the two
hover-target wheel (ruler = time, grid = pitch) with click twins, the hover ghost,
`onHintChanged`, relative 2 px/unit velocity drag, one-click undo pushed **once per gesture**
into a 16-deep ring.

**Rebuilt, because it does not survive the port:** the model (free onsets, arbitrary lengths,
overlapping polyphony, 88 chromatic rows — `Cell`, `cellIndex`, `MAX_STEPS` and the
fixed-array-of-atomics publish scheme all go), the time axis (seconds with a bar grid derived
from `TimeQuantizeOptions`), and the hit testing.

**Two things that must be built and are in neither source proposal:**

1. **Minimum hit width.** A note is drawn at true length but its hit rectangle is ≥ 34 px
   wide, centred on the body; overlaps resolve front-most, then highest-confidence. Without
   this, a note at the default 125 ms duration draws 5 px wide at a 30 s view and the whole
   editing surface fails its own contract.
2. **Pitch nudge on the bar** — `−8ve −1 +1 +8ve` at 40 × 24 — so pitch and time are never
   coupled in one gesture. Move mode's drag is time-only.

**Tests (against `QuarryNotes.h` alone, `juce_core` only):** hit rect is ≥ 34 px at every
zoom; overlap resolution is deterministic; a create-and-size drag produces exactly one note
and one undo entry; an Erase drag across 12 notes is one undo entry; transpose ±8ve clamps at
21 and 108 rather than wrapping; the docked roll never renders fewer than 12 rows at the
minimum window size.

**Definition of done:** Owen corrects a full take with the mouse alone and never reaches for
a keyboard, a modifier or an edge handle.

**14 days** (12 + a 2-day spike buffer).

---

## Milestone 5 — Bulk correction and retroactive re-slice

**Goal.** Correcting a transcription stops being a job.

- **Region select**: one drag along the 34 px ruler. On selection, a bulk-verb row appears on
  the roll bar at ≥ 34 px: **Transpose ±8ve**, **Transpose ±1**, **Quantise**, **Delete**,
  **Delete faded**.
- **Delete faded** (everything below the Conf Floor) is the highest-value single operation in
  the product on dense material.
- **Retroactive re-slice**: drag either edge of the keep bracket over a sealed take's
  waveform; release re-runs the model on the new span (~0.5 s per 30 s).
- The Note Sens / Split Sens / Min Dur rotaries become **live drag-to-update** via
  `retranscribe()` (~1 ms, because the posteriorgrams are retained), with the roll redrawing
  under the cursor.

**Tests:** a bulk verb over N notes is exactly one undo entry; a re-slice preserves
hand-edited notes that fall inside the new span (`origin == 1`) and discards model notes;
`retranscribe()` allocates nothing on the message thread's hot path.

**Definition of done:** Owen fixes a dense capture in bars-per-click rather than
notes-per-minute, and never has to get the first slice right.

**6 days.**

---

## Milestone 6 — Key, the ribbon, and the local description

> **Superseded in part.** The key half landed in the fork ahead of this plan, as
> `Lib/Model/KeyEstimate.h`: one global answer from a duration- and amplitude-weighted
> histogram against Krumhansl-Kessler, shown as **DETECTED** beside the snap controls with a
> **Use it** button. No ribbon, no runner-up, no relative sibling, so the honesty argument
> below is still unanswered and is still the reason to build this properly. What the fork did
> settle is that a raw correlation cannot be shown as a confidence: it never approaches zero
> on material with no key, and a two-note histogram outscores a real scale, so a pitch-class
> support gate and an absolute floor are both required. Keep them.

**Files:** `src/analysis/KeyEstimator.h` (duration- **and confidence-**weighted 12-bin
pitch-class histogram vs Temperley-Kostka-Payne, with Krumhansl and Albrecht-Shanahan
selectable; plus an 8-bar sliding window), `src/analysis/Describe.h` (composes the paragraph
from `AnalysisReport` by string formatting — **always runs**, no network).

Key shows **winner, runner-up and relative sibling**, each a one-click swap, because the
characteristic failure is the relative sibling and a single confident label is dishonest. The
ribbon under the roll turns a modulation into a colour change rather than one wrong answer.

**Also in this milestone: `NumericTextEditor` is deleted from the codebase.** Meter is a
`StepComboBox`. Typing is the one thing the contract forbids.

**Tests:** known-key MIDI fixtures recover the correct key; a piece that modulates at bar 33
produces a ribbon change within ±2 bars; the paragraph is deterministic for a given report.

**Definition of done:** the paragraph reads like a person wrote it, offline, with no API key.

**5 days.**

---

## Milestone 7 — 🟡 Focus filters *(gated on an ablation)*

**Run the ablation before writing a line of it.** Five real YouTube captures Owen chose,
scored by note F-measure against hand-corrected ground truth: (a) raw mix, (b) HPSS-harmonic,
(c) a true audio-domain band-pass. **Nobody has published this comparison**, and it is the only
thing that says whether the chip row is worth a week. One afternoon; it gates six days.

If it passes: `src/dsp/FocusFilter.h` over `juce::dsp::FFT` — median-filtered HPSS plus a
**true audio-domain** band-pass, wired to six 60 × 24 chips (Full / Harm / Perc / Low / Mid /
High) on the SOURCE bar. Note this is a genuinely different operation from the existing
min/max-note control, which only restricts the output loop *after* the CNN has already seen
the contaminated full-band mix (`Notes.cpp:78-88`).

**Sold as transcription aids, never as "stems".** If a chip row of six ever becomes ten
because real stems arrive, they join the same row — there is no separate stems panel.

**Tests:** HPSS on a synthetic mix of a sine and a click train separates them; band-pass is
audio-domain (assert the CNN input differs, not just the output).

**7 days including the ablation.**

---

## Milestone 8 — 🟡 Chords and section boundaries *(gated on measured accuracy, and on Q1)*

`src/analysis/ChordEstimator.h` (beat-synchronous chroma folded from Quarry's own notes vs 25
templates — 12 maj, 12 min, N.C. — with a small Viterbi over a self-transition prior) and
`src/analysis/SectionEstimator.h` (self-similarity matrix over beat chroma, checkerboard-kernel
novelty peaks, **boundaries only**).

**The gate: measure on 20 real captures Owen chose. If chords land under ~60 % root/majmin
agreement with his own ear, the chip row does not ship.** A chord chip that is wrong a third
of the time is worse than no chip. This gate is the mechanism that stops a wrong answer
shipping as decoration, and it applies here regardless of Q1's answer.

Section boundaries are asserted; functional labels are a guess in lighter type with a
one-click 6-item rename popup at 204 px (inside Keys' 340 px no-hover-scroll budget).

**7 days.**

---

## Milestone 9 — 🔴 The Claude read *(BLOCKED on Q1 and Q2)*

`DESCRIBE_ONLINE` chip, **off by default**, sends the `AnalysisReport` as a ~1.5 KB JSON fact
sheet to `claude-haiku-4-5` with an `output_config` json_schema returning
`{headline, paragraph, notable[]}` (~$0.003 a call), on its own `juce::Thread` with a visible
cancel. `claude-sonnet-5` offered as a deeper read. **Any failure silently keeps the local
paragraph** — the network is an upgrade, never a dependency. No audio leaves the machine (the
Claude API does not accept audio input as of July 2026), and the UI says exactly that.

**Blocked because nobody has decided whose API key pays.** A key baked into a shipped binary
is extractable and the cost is unbounded; a user-supplied key is a text field, which the
contract forbids — so it has to be a file chooser pointing at a key file.

**3 days once unblocked.**

---

## Milestone 10 — Video audio in

`okstudio/MediaFoundationReader.h` in the kit: header-only `IMFSourceReader` COM wrapper, the
same shape and size class as `WasapiLoopback.h` (554 lines). Audio half only —
`readAudioAsPcm(File) -> AudioBuffer<float>` + sample rate. Zero third-party code, zero
redistributables, and **no codec patent exposure, because Microsoft already licensed the
codecs**. A dropped `.mp4` / `.mov` / `.mkv` / `.m4a` decodes straight into a take.

Be honest in the pitch: Owen can already get a video's audio by playing it and clicking KEEP.
This is a quality and convenience upgrade — exact, offline, faster than real time, no
re-record, no level matching — not a new capability.

**Ships with the codec-gap plate from day one:** MF cannot open WebM/VP9 or AV1 without free
Microsoft Store extensions, and yt-dlp's default best-quality YouTube output is frequently
exactly that. A 34 px plate says "this video uses a format Windows cannot read — re-save it
as MP4", rather than failing silently.

**Tests:** a known MP4 decodes to the expected sample count; a constant-frame-rate file and a
variable-frame-rate file both produce correct durations; an unopenable file returns a
`juce::Result` rather than crashing.

**6 days.**

---

## Milestone 11 — 🔴 Synthesia scanline *(BLOCKED on 20 URLs from Owen)*

`okstudio/SynthesiaScan.h`, pure `juce_core`, ~300 lines, **no OpenCV and no deep learning**,
fed frames by `MediaFoundationReader`. A Synthesia video is a deterministic *render of a MIDI
file*, so recovering it is de-rendering, not perception.

Algorithm: find the keyboard band as the horizontal region of alternating high/low luminance
in the lower frame; derive per-key pixel columns from the **actual black/white key boundaries**
in that band (never 52 equal divisions — that is the bug that makes the existing Python
prototype unable to emit a single black key, `synthesia_detector.py:120`); sample **one
scanline just above the strike line** each frame (the existing prototype registers onsets when
a block enters the *top* of the frame, `:133` + `:216`, so every onset is early by the full
fall time and every duration is inflated by it); note-on when a column goes background →
saturated colour; colour → MIDI channel for hand separation.

**Clean-room. Do not read `video2midi`'s or `synthesia_to_midi`'s source** — every existing
tool is GPL-3.0 or has no licence at all. The one-paragraph algorithm description plus the
(fixed) prototype in `prototypes/synthesia_detector/` is a sufficient and clean specification.

**The differentiator is mouse-only calibration:** one paused frame, two draggable handles
(strike-line Y, keyboard extent), and a live overlay of the detected key columns. Every
existing tool needed a config file or a keyboard-driven dialog.

**Then the cheap fusion, and only the cheap one:** pitch and onset from the video (exact,
because it is a de-rendered MIDI file), tempo / key / velocity / pedal from the audio pass,
per-note confidence deciding disagreements. A Synthesia render contains no velocity, no
sustain pedal, no tempo map and no time signature — that is the structural reason fusion is
mandatory rather than optional.

**Blocked because the spec is not safe to write without the sample.** Fix the two prototype
bugs in the existing Python first (an hour) and count how many of Owen's 20 succeed. If 3D or
perspective renderers or heavy strike-line particle effects dominate, the scope changes
materially.

**20 days once unblocked.**

---

## Licence decisions required before anything ships

| Component | Question | Status | Blocks |
|---|---|---|---|
| **ASIO SDK** (vendored, `JUCE_ASIO=1`) | Steinberg requires a signed agreement to redistribute a binary containing it. Drop ASIO, or sign? Affects the **kit** too — `AudioCapture` keeps it deliberately | 🔴 **Owen** | M0D |
| **JUCE** | A sold closed-source product with `JUCE_DISPLAY_SPLASH_SCREEN=0` needs a paid licence. NeuralNote never faced this (it is open source) | 🔴 **Owen** | M0D |
| **`spotify/basic-pitch` NOTICE** | Does upstream ship one? Apache-2.0 §4(d) only binds if the Work included one, and if it did, its contents must be reproduced in Quarry's | Unverified — 5 minutes with network | M0D |
| **`DamRsn/NeuralNote` NOTICE** | Same question | Unverified — none exists in this fork's history | M0D |
| **basic-pitch weights** | Apache-2.0 code *and* weights, per Spotify. Trained partly on MAESTRO (CC BY-NC-SA 4.0). Whether permissive weights trained on NC data survive a challenge is **unsettled law**. Owen already has this exposure today | Needs counsel before a **paid** launch, not before a build | Paid launch |
| **Demucs / HTDemucs weights** | Author states research-only (`demucs#327`). Downstream repos relabel them MIT without authority | ❌ **Refused** — see design | — |
| **Open-Unmix UMXL / ADTOF / MT3 checkpoints** | CC BY-NC-SA / CC BY-NC-SA / no licence at all | ❌ **Refused** | — |
| **Essentia / madmom models / Chordino / libKeyFinder / aubio / QM-DSP** | AGPLv3 / CC BY-NC-SA / GPL × 4 | ❌ **Refused** — estimators are written in-house | — |
| **FFmpeg, OpenCV video I/O** | LGPL: EULA reverse-engineering clause must be removed, exact-corresponding source hosted forever. OpenCV's Windows decode path *is* that LGPL DLL | ❌ **Refused** — Media Foundation instead | M10 |
| **Claude API** | Whose key pays: user-supplied key file, baked-in, or no network at all | 🔴 **Owen** | M9 |
| **ONNX Runtime fork** | `tiborvass/libonnxruntime-neuralnote` — a personal March-2023 release, gitignored, the single point of failure for every transcribing product in the line | ⚠️ **Mirror in M0B** | Everything |

---

## Effort summary

| M | Title | Days | Status |
|---|---|---|---|
| 0 | Preflight, spike, rename | 8 | 0D blocked on ASIO + JUCE |
| 1 | The KEEP loop *(run.py usable)* | 8 | |
| 2 | Tempo, and the end of the fictional grid | 4 | |
| 3 | Confidence, honestly | 4 | needs 0B green |
| 4 | The editable roll | 14 | largest estimate risk |
| 5 | Bulk correction + re-slice | 6 | |
| 6 | Key, ribbon, local description | 5 | |
| 7 | Focus filters | 7 | 🟡 gated on ablation |
| 8 | Chords + section boundaries | 7 | 🟡 gated on accuracy + Q1 |
| 9 | The Claude read | 3 | 🔴 blocked |
| 10 | Video audio in | 6 | |
| 11 | Synthesia scanline | 20 | 🔴 blocked |

**Unblocked, ungated core (M0–M6): 43 days.** Everything after that is optional and each
piece can be cut without touching the spine.
