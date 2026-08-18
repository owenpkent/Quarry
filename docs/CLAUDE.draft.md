# CLAUDE.md — Quarry AI Onboarding

> **Status, 2026-07-30, updated 2026-07-31.** This is a *draft* `CLAUDE.md` for the Quarry
> repo, which does not exist yet; it lives in the donor fork until it does. `docs/DESIGN.md`
> and `docs/PLAN.md` are the shape; this file describes that shape in the present tense
> because it is what the code will be, and **every bullet that is not yet code is marked
> `[not built]`**. The markers are about the Quarry repo. The only thing that works today is
> the loopback spike in the donor fork at `../NeuralNoteVideo`, which has since been renamed
> so that its product is also called Quarry, though its checkout path and git remote are
> still `NeuralNoteVideo`. When something lands, delete its marker — do not let this file
> drift into describing intentions as facts.
>
> **The Build & Run section below is the Quarry repo's, not the fork's.** The fork builds with
> its own `run.py` (Windows only, no `--hold`, no `run.ps1`, a 240 s launch deadline) and
> vendors what it needs; see the fork's `README.md`.

## About the Owner

Owen is a wheelchair user with muscular dystrophy who produces music with a single
mouse. Typing is hard — be proactive, make decisions, offer A/B/C choices so he can
answer with one letter. Mouse-only operability is not a preference here, it is the
product.

## What This Is

A JUCE Standalone (+ VST3) that **listens to whatever the computer is playing, keeps the last
five minutes in RAM, and turns any slice of it into editable MIDI plus an honest description
of what it heard.** In the OK Studio line alongside Keys, Lattice, Strata, Contour, Undertow
and Beatform. A fork of NeuralNote (Damien Ronssin, Apache-2.0), which is itself a JUCE port
of Spotify's basic-pitch — the fork is at `../NeuralNoteVideo` and is the donor, not the
product.

**The brain is analytical, and the output is MIDI that leaves on the track.** Owen's
instruments are already in Ableton, so the primary use is Quarry driving *them*: a drag from
the header plate onto a track, or live MIDI out on a configured channel. Ableton's own devices
(Simpler, Operator, Drum Rack) cannot be loaded into any plugin host, so routing is the only
way the brain can ever reach them. Same shape as Strata, generative brain swapped for an
analytical one. `docs/ABLETON.md` is the workflow.

Owen's own words for what "sampling machine" means, verbatim: *"it's for transcribing to midi
and describing audio. see strata."* **Sampling audio INTO the machine.** It is not an
MPC — no drum pads, no slice-and-play, no chopping a break into 16 slices. An earlier design
pass made exactly that mistake.

**The judging question for every feature: does this get better MIDI, or a better description,
out of captured audio?** If it only makes the thing more fun to poke, it is off thesis.

Read `docs/DESIGN.md` first. Read its *Roads not taken* section before proposing anything
that sounds like it should obviously exist — a live scrolling analysis strip, bundled stem
separation, an armed Record button, and visual transcription of piano performance video were
all considered in detail and rejected for reasons that have not changed.

## Build & Run

**Default dev loop: `run.py`.** Double-click it in Explorer (no arguments, no terminal) to
build exactly the `Quarry_Standalone` target and relaunch it. It closes any running Quarry
window politely first (a forced kill skips JUCE's settings write) and holds the console open
on failure so the error is readable. `run.ps1` is a thin shim over `run.py`, so there is one
copy of the logic. It is a copy of Strata's, which is the reference version, with two grafts
Strata does not need: `ensure_onnxruntime()` and the forced `-DLTO=OFF` reconfigure.

```powershell
py run.py              # build + launch Quarry standalone
py run.py --no-build   # just relaunch what is already built
py run.py --hold       # keep the console open on success too
```

Smart App Control is enforced on this machine and dev builds are unsigned, so the *launch* of
a freshly linked exe can be held while Windows vets it. `run.py` waits that out with a counter
on screen — **1200 seconds**, Ctrl+C to stop waiting. If you find a 240-second timeout in this
repo, it is the fork's weaker version and it should be replaced, not tuned.

Full build (VST3 + install to the DAW):

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DQUARRY_COPY_PLUGIN=OFF -DLTO=OFF
cmake --build build --config Release --target Quarry_VST3 Quarry_Standalone
```

Artifacts: `build/Quarry_artefacts/Release/{VST3,Standalone}/`. Needs JUCE at `../JUCE`.

**The kit is not a checkout requirement.** It is a private repo, so anything that needs a
sibling `../okstudio-juce-kit` to build leaves a public clone unable to build at all. The
fork settled this by **vendoring** the headers it uses into `ThirdParty/okstudio/include/`,
pinning the source commit in `ThirdParty/okstudio/UPSTREAM.txt`, re-syncing with
`py tools/sync_okstudio.py`, and warning at configure time when a kit checkout that *is*
present has drifted from the vendored copy. Quarry should do the same, or make the kit
public. A sibling checkout is for syncing and for editing the kit, never for building.

**Three build facts that are not choices, and that every new dependency has to live with.**
They are all properties of one prebuilt library — a minimal ONNX Runtime built once, in March
2023, against basic-pitch's operator set:

- **`-DLTO=OFF` is mandatory.** `onnxruntime.lib` is compiled `/GL` by one specific MSVC
  version and linking fails outright with `C1047` otherwise. This is why the fork's
  `build.bat` cannot build the fork.
- **The static MSVC runtime (`/MT`) is forced on the whole binary.** Any library Quarry adds
  must also be `/MT` or you get a wall of `LNK2038` with no cause named.
- **`build/` is expensive to delete.** The expanded static lib is 2.9 GB. The *download* is
  ~100 MB — do not repeat the "2.9 GB download" line, it has been getting a cheap existential
  fix deferred (see the mirror invariant below).

## Architecture

Read `docs/DESIGN.md` first, then `docs/PIPELINE.md` before touching anything downstream of
the model, and `docs/MODEL_LICENSES.md` before adding any weight file at all. Load-bearing
ideas:

- **KEEP is the product.** `[not built]` Quarry listens from launch and keeps the last 300
  seconds in a RAM ring. One click on a 190 × 76 plate turns the last N seconds into a take,
  starts the analysis, and puts a drag-ready `.mid` behind the header plate. **Nothing is
  armed in advance**, because the failure mode of every capture tool ever built is that you
  were not recording when the good thing happened. The plate is *structurally* undeletable —
  no fold state, no detach flag, no `LayoutState` entry. A one-click contract that any UI state
  can hide is not a contract. **Analyse fires automatically on KEEP**; there is no second
  button.
- **Nothing touches disk until KEEP.** `[not built]` The ring is RAM only, 115 MB at the 300 s
  ceiling, and the UI says so. Always-on disk recording is ~1.4 GB an hour and a materially
  different product with a different consent story.
- **The ring's time axis must be real time.** `[not built]` WASAPI loopback does not
  necessarily deliver packets while the endpoint is idle; silence can arrive as a *gap*. If a
  gap is closed up rather than padded, "KEEP LAST 30s" returns 30 seconds of audio spanning
  several minutes of wall time — a splice that will look like a transcription bug. Every ring
  block carries its QPC timestamp and gaps are **zero-padded**. This is tested against a
  synthetic writer, not code-reviewed.
- **Three threads, and the third one is new to the line.** `[not built]` The **audio thread**
  plays back source and audition, reads an immutable POD note window through a `NoteFeed`
  (Strata's `ScoreFeed` shape: three fixed slots, two atomics, memcpy then release-store), and
  makes no decisions. The **message thread** does UI, layout, and the ~1 ms `retranscribe()`
  fast path. The **analysis thread** is a `juce::ThreadPoolJob` on a one-thread pool running
  the staged pipeline, checking `shouldExit()` between stages and publishing each finished
  stage back via `MessageManager::callAsync`. Strata composes on the message thread because it
  costs 0.3 ms; a transcription plus a description costs seconds, so it cannot live there.
  **Audio never crosses `NoteFeed`** — its slots are POD by value.
- **`processBlock` order is Strata's, exactly**, including that the outgoing MIDI copy is the
  **last** thing in the block, after every producer has written its own buffer.
- **Transcription can never be live, and the UI never implies it is.** The constant-Q front
  end needs over a second of audio before its low bins mean anything and note events resolve
  backwards from the end. Capture then transcribe, always. This is *not* a reason to make the
  user press a second button — see KEEP.
- **Inference is split across two runtimes and neither is optional.** ONNX Runtime runs only
  the CQT + harmonic-stacking front end (one 175 KB `features_model.ort` graph); RTNeural runs
  all four CNN heads (contour, note, onset-input, onset-output) frame by frame from JSON
  weights. It is single-threaded **by construction** (`SetInterOpNumThreads(1)`,
  `SetIntraOpNumThreads(1)`, a serial RTNeural loop), so timings do not improve with cores.
  Measured: 3.1746 s of audio in 49.4 ms total. Speed is a non-issue; do not optimise it.
- **The posteriorgrams are retained after transcription and they are the description
  pillar.** Contours 264 × N, notes 88 × N, onsets 88 × N at 86.13 fps, free to read. That is
  why `retranscribe()` re-derives notes in about a millisecond — which is why the sensitivity
  knobs are live drag-to-update controls with the roll redrawing under the cursor. Keep that
  fast path front and centre; it is already technically free.
- **`amplitude` is three fields, not one.** `[built]` The model's mean note-posteriorgram
  value is *confidence*. In the fork it was also written straight into exported MIDI velocity
  and into the preview synth, so the fork's exported dynamics **were** the model's uncertainty.
  `Notes::Event` now carries `velocity` and `onsetConfidence` alongside it. Velocity is
  harmonic-band CQT energy at the attack, not span RMS as first specified: span RMS contains
  every other note sounding at the same time. `Lib/Model/NoteVelocity.{h,cpp}`.
- **Every estimator is arithmetic we wrote, in a `juce_core`-only header.** `[not built]`
  `KeyEstimator.h`, `TempoEstimator.h`, `ChordEstimator.h`, `SectionEstimator.h` — about 800
  lines total, over data already in RAM, unit-tested by a test exe linking `okstudio_kit`
  alone. This is not NIH: the entire classic C++ MIR stack is copyleft (Essentia AGPLv3,
  madmom's models CC BY-NC-SA, Chordino GPLv2+, libKeyFinder GPLv3, aubio GPL, QM-DSP GPL) and
  none of it can go into a closed-source VST3. Writing them is cheaper than the licence
  conversation.
- **Tempo is a correctness prerequisite, not a description feature.** `[not built]` The fork
  reads BPM and time signature off the DAW playhead only and defaults to 120 / 4-4, which means
  in standalone — Quarry's primary mode — the quantise grid is fictional and every exported
  `.mid` lands in Live at the wrong tempo. The estimate feeds `TimeQuantizeInfo::bpm` as the
  standalone source; the playhead stays the override when hosted. The `÷2` / `×2` chips are
  permanent furniture on the ANALYSIS bar, never behind a menu, because octave errors are ~20 %
  of tempo failures and there is no algorithmic fix.
- **The four descriptions are ranked and the ranking is a decision.** Owen picked all four
  when asked; that is a wish list, not a priority order. Confidence first (already computed,
  and the only one that makes the *MIDI* better), tempo second (a bug fix), key third (cheap,
  and it auto-fills the snap dropdown that already exists), and chords / section labels / the
  paragraph last, **gated on measured accuracy on 20 real captures Owen chose**. A chord chip
  that is wrong a third of the time is worse than no chord chip.
- **Capture is standalone-only.** `[landed in the fork]` Loaded as a VST3 in Live with System
  Audio selected, Quarry would hear Live's master bus, including its own audition voice and
  whatever MIDI it is driving. There is a second reason found while building it: an
  exclusive-mode driver serves one client at a time, so a device the plugin opened for itself is
  a device the host can lose. A hosted plugin therefore never constructs an `AudioDeviceManager`, never
  lists drivers or devices, hides those pickers, and records the audio the host sends it. The
  plugin build hard-disables the loopback source and explains why on a 34 px plate. The VST3
  exists to *emit* MIDI, Strata's shape.
- **Chrome is Keys', copied verbatim.** `[not built]` `SectionBar.h`, `DetachedWindow.h`,
  `StepComboBox.h` and the `Holder` / `Section` structs, namespace `keys` → `quarry`. Four
  sections in a `std::array<Section, 4>` wired by one lambda: **SOURCE, ROLL, ANALYSIS,
  OUTPUT**. Add a section by adding an entry, not by copying a code path. Layout state — folds,
  detach flags, detached window frames — lives in `QuarryProcessor::LayoutState` on the
  **session ValueTree, never as parameters**: none of it changes a note, and exposing it to
  host automation only adds ways to break a session.
- **`StepComboBox`, not `ComboBoxAttachment`, for anything showing a rounded value.** A
  `ComboBoxAttachment` finishes through `setSelectedId`, which early-returns when the id has
  not moved, so picking the item already showing is a **dead click on a lit control** — the one
  thing a mouse-only plugin must never have. Any Quarry combo showing a nearest or rounded
  value (confidence threshold, tempo candidate, quantise strength) uses `StepComboBox` with an
  explicit write.
- **The roll is Lattice's, and only half of it ports.** `[not built]` The `Geom`/`rowAt`/
  `clampScroll` math, the constant `rowH = 36.0f` with both `static_assert`s, the two
  hover-target wheel with click twins, the hover ghost, the relative 2 px/unit velocity drag,
  `onHintChanged`, and one-gesture-one-undo all come over. The **model does not**: Lattice is a
  quantised Start/Hold cell grid, one note per (row, step); a transcription is free onsets,
  arbitrary lengths, overlapping polyphony over 88 chromatic rows. `Cell`, `cellIndex`,
  `MAX_STEPS` and the fixed-array-of-atomics publish scheme all go. `QuarryNotes.h` keeps
  Lattice's *file shape* and its *function names* (`noteStartCovering`, `noteLengthAt`,
  `collectNotes`) so the canvas port is mechanical. **Verify the extraction before budgeting
  it**: `RollCanvas` holds a `LatticeProcessor&` with 15 methods over 37 call sites, all
  step-grid shaped.
- **Fixing a wrong note is its own subsystem, and note-at-a-time is the last resort.**
  `[not built]` A 3-minute capture through a 16,782-parameter instrument-agnostic CNN on
  out-of-distribution material yields hundreds of wrong notes. In order: *Next uncertain bar*
  (one click, scrolls and selects) → **Delete faded** (one drag on the Conf Floor rotary, then
  one click) → **region select by dragging the ruler** with bulk verbs (transpose ±8ve,
  quantise, delete) → single-note edit. See `docs/DESIGN.md` → *How a wrong note gets fixed*.
- **Two rules make single-note editing possible at real zoom.** `[not built]` (1) A note's
  **hit rectangle is at least 34 px wide** regardless of its drawn length — at a 30 s view
  across ~1300 px, a 125 ms note draws 5 px. Lattice gets away with `minColW = 36` only because
  its steps are quantised. (2) **Pitch and time are never coupled in one gesture** — the pitch
  nudge row (`−8ve −1 +1 +8ve`) is the pitch path, and Move mode's drag is time-only, because
  basic-pitch's signature failure is an octave slip and fixing it with a free 2D drag inside a
  36 px row introduces a timing error you then fix in a second mode.
- **`rows <= h / 34` is a gate, and Quarry's version of it is 12.** `rowH` is never a
  quotient; the pitch axis scrolls and never compresses to fit. Contour shipped 30 px default
  rows and violated the founding constraint. **If a new section, row or lane would take the
  docked roll below 12 rows at the minimum window size, that thing does not ship.**
- **Everything docks. Detach exists and is never required.** `[not built]` Launching into two
  or three overlapping OS windows makes window management the user's first task, and window
  management with one mouse is the most expensive thing you can ask for. `ensureWindowReachable()`
  runs **after** `addToDesktop()` on every top-level window, because a saved frame from another
  display leaves a title bar off-screen, and for a mouse-only user that is a window that can
  never be moved or closed again.

## Invariants (don't break)

- **Mouse-only UI**: single left-click, drag or scroll; targets ≥ ~34 px; no keyboard, no
  double-click, no modifiers. Every drag target ships a `<` `>` stepper twin at the full 34 px
  that reaches every value the drag can. Every wheel gesture has a click twin. **There are no
  edge handles anywhere in Quarry** — the verb is a mode and the target is the whole note body.
  Right-click is an optional accelerator only and every right-click path has a left-click twin;
  Quarry inherits no exceptions from Keys and should not invent any without Owen's explicit
  say-so. Every feature request answers "how is this reached with one left-click?" before it is
  worth designing.
- **No typing, anywhere.** The fork's `NumericTextEditor` (tempo, both time-signature fields)
  and `QuarryMainView::keyPressed` (space, shift+space, shift+backspace, r, m, c) are
  deleted rather than restyled. Every numeric value is a drag with a stepper twin or a
  click-through list. If a feature needs a string from the user, it needs a file chooser or a
  fixed list instead.
- **No popup taller than 340 px** (9 rows + 2 separators at `withStandardItemHeight(34)`).
  JUCE answers an over-tall menu by column-breaking or hover-scrolling, and a hover-scrolling
  popup **cannot be operated with one mouse** — hovering the arrow scrolls, moving to click
  scrolls the item away. A JUCE section header costs 1.5 items (51 px), not 1.
- **Keep the MIDI input bus** (`NEEDS_MIDI_INPUT TRUE`, set by `okstudio_add_plugin`).
  Ableton refuses to load an instrument without one; the failure is silent-looking ("This VST3
  plug-in could not be opened") and pluginval does not catch it. Only a real Live load test does.
- **Audio thread: no allocation, no locks, no decisions.** It reads a POD snapshot at block
  top, mixes source and audition, drains a `MidiMessageCollector`, and copies MIDI out last.
  The fork takes a `ScopedLock` on the audio thread from two driver threads
  (`SourceAudioManager.cpp:68`) — that does not survive the port.
- **Parameters are append-only.** The one-time rename (`OwKe`→`OKSt`, `NNVd`→`Quar`, every id
  string) happens once, at repo creation, `versionHint 1`. After the first release, never
  reorder, never remove, changelog loudly.
- **Every accessible name is unique.** `scripts/capture-window.ps1` drives the app through UI
  Automation **Invoke by name** plus `PrintWindow(PW_RENDERFULLCONTENT)` — never
  `SetForegroundWindow`, never `SetCursorPos`, never synthesized clicks (Owen is often using the
  machine, and a mis-capture can grab his private windows). **UIA takes the first name match**,
  so every `SectionBar` is `setTitle(caption + " section")`, every Detach button is
  `setTitle("Detach Roll")` flipping to `"Re-dock Roll"`, and every `<` / `>` half gets its own
  real title.
- **The accent is per editor instance, never a global.** A DAW loads every Quarry into one
  process; a mutable global repaints every track's window at once. Hang it off the per-editor
  LookAndFeel and resolve via `skin::accentOf()`. Two traps: `setAccent` must re-apply every
  JUCE ColourId baked from the accent, and every *second* LookAndFeel instance must be
  re-tinted alongside the first.
- **Never ship a model weight without reading its weight licence, as distinct from its code
  licence.** `docs/MODEL_LICENSES.md` lists every artefact, its stated licence, its upstream
  repo and its training data. Refused outright and not reopenable without Owen: **Demucs /
  HTDemucs weights** (the author states in `facebookresearch/demucs#327`, verbatim, *"The model
  weights are not covered by the MIT license, and are provided only for scientific purposes"* —
  and downstream repos that relabel them MIT have no authority to), **Open-Unmix UMXL**,
  **ADTOF**, **MT3 checkpoints** (no stated licence at all; absence of a licence is not
  permission), **Essentia**, **madmom's `.pkl` models**, and every GPL MIR library.
- **Apache-2.0 §4(b) and §4(c).** Every file derived from NeuralNote keeps its
  `Created by Damien Ronssin` header (stripping it breaches §4(c)) **and** carries
  `// Modified 2026 by Owen Kent for Quarry.` `NOTICE` and `LICENSE` ship at the root. §6 grants
  no trademark rights, which is why the name is Quarry.
- **The ONNX Runtime tarball must resolve from a mirror Owen controls.** It is a ~100 MB
  artifact published once, in March 2023, on one individual's personal GitHub release
  (`tiborvass/libonnxruntime-neuralnote v1.14.1-neuralnote.1`), and it is gitignored so nothing
  local preserves it. If it disappears, **nothing in the OK Studio line that transcribes will
  ever build again.** `OKSTUDIO_ONNXRUNTIME_CACHE` points at a stable directory outside every
  build tree.
- **That runtime forecloses in-process model additions, permanently.** It is a
  `--minimal_build --include_ops_by_config --enable_reduced_operator_type_support` build against
  basic-pitch's op set, so it physically cannot load any other ONNX model, and you cannot link
  two ONNX Runtimes into one binary. It is also `--disable_exceptions`, so a failed
  `Ort::Session` construction **aborts the process** rather than throwing. Any future model is a
  runtime rebuild with a union op config, or a sidecar process. **Plan for sidecar.**
- Conventional commits (`feat:`, `fix:`, `docs:`, `chore:`). Update `CHANGELOG.md` under
  `[Unreleased]` with every user-visible change. Never add AI attribution to commits.

## Set expectations, do not let it look broken

basic-pitch is a 16,782-parameter instrument-agnostic CNN trained on solo and small-ensemble
material (MAESTRO, GuitarSet, MedleyDB stems, Slakh, vocals). Its own paper puts it at note-F
70.9 on MAESTRO piano against Onsets-and-Frames' 95.2, and **10.5 against 36.4 once offsets
count** — so note *durations* out of piano material are close to meaningless, and any UI that
reports articulation or rhythmic feel from them is reporting noise. It beats the guitar
specialist on GuitarSet (84.0 vs 76.3), so it is not uniformly weak; it is weak on exactly the
material a YouTube-capture product gets fed. Expect few or no notes on drum-heavy and dense
mixes. The model is hard-locked to 22050 Hz mono, so everything above ~11 kHz is gone before
inference and the sum-and-divide downmix destroys stereo information and can partially cancel
phase-opposed content. Sub-bass is structurally weak. **None of this is fixable inside Quarry;
it is a ceiling to design around, and the UI's job is to be honest about it rather than to look
faulty.** Confidence display must cover offsets separately from onsets or it will lie.

## Sibling projects (same owner, same conventions)

`../okstudio-juce-kit` — shared header-only kit, and **private**. `Theme.h`, `Scales.h`,
`StateHelpers.h`, `MouseOnly.h`, `Updater.h`, `Mcp.h`, `RotaryKnob.h`; `Transcribe.h` +
`src/basicpitch/` (the engine, behind `-DOKSTUDIO_KIT_BASICPITCH=ON`); `AudioCapture.h` +
`CaptureMath.h` + `WasapiLoopback.h` (system-audio loopback via hand-written COM, because
JUCE 8.0.8 has none); `cmake/OKStudioPlugin.cmake` → `okstudio_add_plugin()`. **Fix shared
behaviour there, not here**, then re-sync the consumers that vendor it. Because the repo is
private, a consumer that must be buildable from a public clone vendors the headers it needs
rather than depending on this checkout; the fork's `ThirdParty/okstudio/` plus
`tools/sync_okstudio.py` is the pattern. `AudioCapture.h` is deliberately *not* on
`okstudio_kit`'s link line — it needs `juce_audio_devices` and `juce_audio_formats`, and the
failure mode is a link error, not a compile error.

`../Keys` — the line's **reference consumer and UI standard**. Its Section / SectionBar /
DetachedWindow / LayoutState system, its bar chips, its knob bank, its `capture-window.ps1` and
its CMake/test wiring get copied into every new product, Quarry included.

`../Lattice` — mouse-only polyphonic piano roll, and **Quarry's single most relevant sibling**.
Its canvas geometry and gesture vocabulary are Quarry's roll; its cell-grid model is not.

`../Strata` — generative ambient machine. **The threading model to copy**: a message-thread
composer that allocates freely, an audio-thread player that makes no decisions, and a
three-slot two-atomic `ScoreFeed` as the only bulk crossing. Also the reference `run.py`, and
`docs/SC.md` is the precedent for writing down a shelved road so it is not reopened from
scratch — Quarry's equivalent is `docs/DESIGN.md` → *Roads not taken*.

`../NeuralNoteVideo` — the donor fork (Apache-2.0), whose product is now also called Quarry;
only the checkout path and the git remote still say NeuralNoteVideo. Kept checked out for
reference, for the working loopback capture, and for the Synthesia prototype in
`prototypes/synthesia_detector/` and `VIDEO_ENHANCEMENT_PROPOSAL.md`. **Both of those contain
claims that are wrong**: the prototype cannot emit a single black key
(`synthesia_detector.py:120`) and registers onsets when a block enters the top of the frame
rather than the strike line (`:133` + `:216`); the proposal's Path B claims performance-video
transcription is "direct physical ground truth" when the published numbers put visual-only at
onset F1 0.68 against audio-only's 0.877 on the same clips. The proposal now carries a
correction banner saying so. Do not treat either as a baseline.

Also in the line: `../Contour` (drawn melodic contours), `../Undertow` (bass), `../Beatform`
(drums), `../Hex`.
