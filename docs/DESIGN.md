# Quarry — Design

*The ultimate sampling machine. Sampling audio **into** the machine, not chopping it up
inside one.*

Status: design settled 2026-07-30. Nothing in `c:/Users/owenp/dev/Quarry` is built yet;
`c:/Users/owenp/dev/NeuralNoteVideo` is the donor fork and the vehicle for the day-one spike.
Build order lives in `docs/PLAN.md`.

Updated 2026-07-31: the fork's product has since been renamed to Quarry, so "Quarry" names
both the shipped fork and the repo designed here. Everything below describes the repo that
does not exist yet, except where a note says a piece landed in the fork first.

---

## What This Is

A JUCE Standalone (+ VST3) that **listens to whatever the computer is playing, keeps the
last five minutes in RAM, and turns any slice of it into editable MIDI plus an honest
description of what it heard.** The MIDI leaves as a drag onto an Ableton track, or live
out the plugin's MIDI bus, to drive Simpler, Operator, Drum Rack — instruments that cannot
be loaded into a plugin host, so routing is the only road to them. Same shape as Strata,
analytical brain instead of a generative one.

Three sources, in the order Owen actually uses them:

1. **System audio loopback** — a browser tab, YouTube, anything the endpoint mixes.
2. **A file dragged in from disk** — audio, and (later) the audio track of a video.
3. **Mic / interface inputs** — listed because `okstudio::capture::AudioCapture`
   enumerates them for free, never the default, never advertised.

Four things it says about what it heard, ranked by how much they are worth (this ranking
is a decision, not a wish list — see *Load-bearing ideas*):

1. **Per-note confidence.** Which notes the model is unsure about, and therefore which
   bars to go fix by hand. Computed today and, until 2026-08-18, thrown away: the Transcribe
   page now spends the space the piano roll used to hold on a per-bar confidence strip built
   from it, with the key, tempo, meter, note count and length over the top. The roll is a
   toggle rather than the default, because it was a picture of the export and this is a
   judgement of it. See `TranscriptionSummary`, and the CHANGELOG entry for what the strip
   is measured against - the tiers are relative to the take's own median, since the decoder
   derives its thresholds per take and absolute cutoffs painted every bar red.
2. **Tempo and meter.** Not a description feature at all — a correctness prerequisite,
   because without it every exported MIDI file carries a fabricated 120 / 4-4 tempo map.
3. **Key**, with its runner-up and its relative sibling always on screen, plus a sliding
   key ribbon so a modulation shows as a colour change rather than as one wrong answer.
4. **Chords, section boundaries, and a plain-English paragraph.** Last, gated on measured
   accuracy, and cut without ceremony if they do not earn their pixels.

**Stem separation is not on that list**, and *Licence constraints* explains why it cannot
be. What ships instead is a focus pre-filter (harmonic / percussive / band), sold under
that name, never as "stems".

---

## Why it exists next to Ableton's own Convert-to-MIDI

Live has had Convert Harmony / Melody / Drums to MIDI since 9. It is one right-click, it
is free, and it is already installed. Quarry has to be better on axes Live does not
compete on at all:

- **Live cannot capture.** Convert-to-MIDI works on a clip that is already in the Session
  view. Getting a YouTube tab into a Live clip is a loopback device, a routing decision, an
  arm, a record, and a resample — before you have a clip. Quarry's whole first pillar is
  that the audio is already in the buffer before you decided you wanted it.
- **Live tells you nothing about its own confidence.** Convert-to-MIDI returns notes with
  no uncertainty attached. Quarry's per-note confidence, per-bar strip and *Next uncertain
  bar* button exist because the honest answer to "is this transcription right?" is "these
  eleven bars are, those three are not". That is the single most valuable thing in the
  product and Live has no equivalent.
- **Live's drum conversion is three classes.** Kick, snare, hi-hat. So is every
  commercially-licensable open drum transcriber. Quarry does not compete there and says so.
- **Live cannot be driven with one mouse comfortably.** Convert-to-MIDI produces a clip you
  then edit in Live's piano roll, which scores 3 out of 10 on the line's own mouse-only
  piano-roll study: keyboard-only cells, and the primary length target is an undocumented
  1-pixel edge. Quarry's roll is Lattice's — body-only targets, no edge handles, a 34 px
  floor enforced by `static_assert`.
- **Live never describes anything.** No key, no chord chart, no section boundaries, no
  paragraph.

Where Live is genuinely better: it is in the session already, and its drum conversion is
tuned for exactly the material Owen would otherwise waste a Quarry pass on. Quarry should
never claim otherwise, and the UI should not pretend basic-pitch does drums.

---

## The screen, section by section

**One window. Four sections. The roll is the surface you spend your time in.**

Default 1440 × 1000, minimum 1180 × 760, resizable. In standalone the editor asks for
1440 × 916 because JUCE's options bar eats 84 px it does not account for in the window
height (Lattice hit this at `Lattice/src/PluginEditor.cpp:233-237`).

Structural colour from `okstudio::theme`; Keys' `skin` namespace copied wholesale
(`bgTop 0xff17181c`, `panel 0xff1a1c21`, `well 0xff101216`, `control 0xff262a31`,
`radius 6`, `panelRadius 8`, `ui()` / `uiSemi()` / `micro(10)`); the per-editor eight-choice
Accent machinery resolved through `skin::accentOf()` — **never a global**, because a DAW
loads every instance into one process and a global repaints every track at once.

Sections are a `std::array<Section, 4>` wired by one `wire(...)` lambda, copied from
`Keys/src/PluginEditor.cpp:142-196`. `SectionBar` is 34 px and **only its left ~92 px
`foldZone()` folds it** — do not "fix" that narrow target; it exists because a click aimed
at a bar control that misses by a few pixels would otherwise hide the thing being reached
into.

### HEADER STRIP — 76 px. Not a Section. Never folds, never detaches, never scrolls.

```
[QUARRY]                                    [ progress 240×8 ]  [MIDI 220×62] [KEEP 190×76]
 LISTENING 14:03                            Analysing take…     DRAG TO LIVE   KEEP LAST 30s
[accent 34×34]
```

- **KEEP, 190 × 76, right edge, 16 px inset.** The largest target in the product and the
  only one that is *structurally* undeletable: it has no fold state, no detach flag, no
  entry in `LayoutState`. The one-click contract cannot be a contract if any UI state can
  hide it. Its caption tracks the keep window live ("KEEP LAST 30s").
- **The MIDI drag plate, 220 × 62, immediately left of KEEP.** It lives in the header and
  not in OUTPUT for the same reason: the two things Quarry is *for* — grab it, and get it
  into Live — must both be reachable with every section folded. `DraggingHandCursor` on
  hover, `performExternalDragDropOfFiles` on `mouseDown`, greyed until a take has notes.
- Progress bar 240 × 8 with a per-stage caption, and a 140 × 34 **Cancel** that replaces
  the caption while a job runs.
- The 34 × 34 accent swatch, and "LISTENING mm:ss" / "PAUSED" / "SOURCE LOST" in micro caps.

### SOURCE — bar 34, content 96. Auto-folds after the first take.

Bar, right to left, 24 px chips laid out `removeFromRight(w).withSizeKeepingCentre(w-2, 24)`:
`Detach 90` | source `StepComboBox 220` (item 1 is always **System Audio (loopback)**;
persisted by `Source::id()` = `typeName|deviceName`) | peak meter `130 × 24` |
Listen/Pause toggle `72 × 24`.

The last three **never hide on fold**. Keys' rule: a bar control hides with its section iff
it acts on content that is gone, and stays live iff it is the only remaining reach to
something still running. A listener you cannot see, cannot stop, and cannot re-point is
exactly that.

Content 96 px, one row, 12 px gutters:

- **Rolling waveform**, full width minus the takes rail, 96 px tall, scrolling right-to-left
  with NOW pinned at the right edge, 60 s visible, ticks at −60 / −45 / −30 / −15 / NOW.
- **The keep bracket**: a translucent accent overlay pinned to the right edge covering the
  last N seconds, seconds drawn inside it in micro caps. Its left edge is a **34 × 96
  vertical grab bar** — 34 px wide, 96 px tall, deliberately over budget.
- **Takes rail** on the right, horizontal scroll, newest first, each take a `148 × 80` chip:
  a 148 × 40 thumbnail, "14:03 30s" in micro caps, a state dot (amber analysing / green done
  / warn if > 25 % of notes are low confidence). One left-click loads it into the roll.
- The whole content rectangle is a `FileDragAndDropTarget` with a 2 px dashed
  `skin::outline` and a centred `textDim` caption, "or drop an audio or video file".

Below the waveform, riding the fold boundary, a 34 px row: five duration chips
**8s / 15s / 30s / 60s / 120s** at 64 × 34 in one radio group, then a 72 px stepper cell
split into two 34 × 34 halves (`<` / `>`, 1 s steps, each with a real `setTitle()`, because
four buttons reading "<" are four identical accessible names and UI Automation takes the
first match).

### ROLL — bar 34, ruler 34, grid takes all the slack. The tall one.

Bar, at 24 px: `Detach 90` | five mode chips at `68 × 24`, `radioGroupId 1001` —
**Move / Length / Velocity / Confidence / Erase** | pitch nudge `−8ve  −1  +1  +8ve` at
`40 × 24` each | `Undo 84` (greys via `onUndoStateChanged`) | Octave stepper `72`.

Grid:

- Left gutter 56 px, note names, white/black key geometry transplanted from the fork's
  `PianoRoll.cpp:150-186` (that part of it was correct).
- **Ruler 34 px** — not 24. It has to be 34 because a **drag along the ruler is the region
  select**, and a region select is a core action, not an accelerator. The wheel over the
  ruler scrolls time; the wheel over the grid scrolls pitch. Two hover targets, no modifier,
  and both have click twins (Octave stepper, Bars stepper on the detached panel) so the
  wheel is only ever an accelerator.
- `rowH` is a **constant 36.0f**, never a quotient, with two `static_assert`s against
  `okstudio::ui::minHitPx`. The pitch axis scrolls; it never compresses to fit. `rows <= h/34`
  is a gate — Contour shipped 30 px rows and violated the founding constraint.
- Notes are **bodies only, no edge handles anywhere.** Confidence is the body's own shading
  (cool/green confident → warm/amber uncertain, Melodyne's convention). Confidence mode
  writes the number into the body when it is wide enough, exactly as Velocity mode does.
- **A per-bar confidence ribbon, 8 px, painted along the top of the grid, non-interactive.**
  A 3-minute track is ~90 bars; across 1400 px that is a 15 px cell, under the floor on the
  axis that matters for pointing at one. So it is a heat ribbon you *read*, and the thing
  you *click* is the button below.
- Floating bottom-right over the grid, always visible: **Next uncertain bar, 190 × 34.**
- Floating bottom-left, a hint strip painted over the grid (no height cost): what the cursor
  is over and what a click would do, from `onHintChanged`.

**Height budget, and it is a gate.** At the 1440 × 1000 default with SOURCE and ANALYSIS
folded: 916 usable − 76 header − 34 SOURCE − 34 ANALYSIS − 34 OUTPUT − 68 roll chrome
= **670 px of grid = 18 semitone rows.** At the 1180 × 760 minimum it is **12 rows.**
*If a new section, row or lane would take the docked roll below 12 rows, that thing does
not ship.* Detach is available (`{1600, 1000}`, 26 rows) and is never required.

### ANALYSIS — bar 34, content 120. Default **folded**, docked, never detached by default.

Bar, at 24 px, and none of these hide because they are the readout:
key plate `96 × 24` ("F# min") | relative-swap chip `34` | tempo plate `64 × 24` ("92.0") |
**`÷2` and `×2` at 44 × 24 each** | meter `StepComboBox 48 × 24`.

The `÷2` / `×2` chips are permanently visible and never behind a menu. Octave errors are
roughly 20 % of tempo failures and there is no algorithmic fix, so the correction is
first-class furniture. Clicking either re-derives the bar grid, the quantise target and the
exported tempo map immediately.

Content 120 px, three 34 px rows + gutters:

1. **Key** — winner large, runner-up and relative sibling beside it in `textDim`, each a
   one-click swap. A 28 px key ribbon under it from the 8-bar sliding window, so a
   modulation is a colour change.
2. **Chord chart** — chips at `72 × 34` aligned to the bar grid, wheel-scrolls, one click
   cycles a chip to the next-most-likely candidate. *(gated — see Load-bearing ideas)*
3. **Confidence summary + Describe** — "18 % of notes low-confidence, concentrated in bars
   17–24", then `Describe 160 × 34` which expands the paragraph into this section's content
   at 34 px line height. *(no separate DESCRIBE section: it would cost a bar and 34 px of
   roll for one paragraph)*

Section boundaries are drawn as vertical rules **on the roll's ruler**, labelled A / B / A′
/ C, with the functional guess in lighter type. One click on a label opens a 6-item popup at
`withStandardItemHeight(34)` = 204 px — inside Keys' 340 px budget, because JUCE answers an
over-tall menu with hover-scrolling and a hover-scrolling popup cannot be operated with one
mouse.

### OUTPUT — bar 34, content 78. Default folded (the drag plate is in the header).

Bar: `Detach 90` | MIDI channel-mode `StepComboBox 180` (Merged / One channel per focus
slice) | Quantise-to-grid chip `90` | Send-live toggle `100`.

Content 78 px, when unfolded: `Export .mid… 160 × 34`, audition transport `62 × 62`, and
two `okstudio::RotaryKnob` at 60 px (Source gain, Audition gain) in a `KnobBank`-shaped row
at `knobRowH = 110`'s proportions, each with a 34 px label button and a 72 px `<` / `>`
stepper cell beneath.

The transcription rotaries (Note Sens, Split Sens, Min Dur, Min Note, Max Note, Conf Floor,
Quantise, Audition Mix) live on the **detached ROLL window's** knob bank, not in the docked
layout. Eight 60 px rotaries plus their steppers is 110 + 34 px of height, and the docked
roll does not have it to give.

### Accessibility plumbing

Every `SectionBar` calls `setTitle(caption + " section")`. Every Detach button is
`setTitle("Detach Roll")`, flipping to `"Re-dock Roll"`. Every `<` / `>` half gets a unique
real title. `scripts/capture-window.ps1` is copied from Keys unchanged apart from the exe
path; it drives the app through UI Automation **Invoke by name** plus
`PrintWindow(PW_RENDERFULLCONTENT)` — never `SetForegroundWindow`, never `SetCursorPos`,
never synthesized clicks (Owen is often using the machine). UIA takes the first name match,
so uniqueness is load-bearing rather than cosmetic.

---

## Load-bearing ideas

- **KEEP is the product.** Quarry listens from launch and keeps the last 300 seconds in a
  RAM ring. One click on a 190 × 76 plate turns the last N seconds into a take, starts the
  analysis, and puts a drag-ready `.mid` behind the header plate. **Nothing is armed in
  advance**, which matters because the failure mode of every capture tool ever built is that
  you were not recording when the good thing happened. Armed Record costs three clicks
  bracketed by two moments of timing; KEEP costs one click and no timing at all.
- **Nothing touches disk until KEEP.** The ring is RAM only. 300 s of stereo float at
  48 kHz is 115 MB, and the UI says so. Continuously writing everything the machine plays is
  ~1.4 GB an hour, wears the SSD, and is a materially different product with a different
  consent story.
- **Analyse fires automatically on KEEP.** There is no second click. The technical fact —
  the constant-Q front end needs over a second of audio and resolves note events backwards
  from the end, so transcription can never be live — is true and is the reason there is no
  live note display. It is *not* a reason to make the user press a second button.
- **Retroactive re-slice is what makes one click safe.** Drag either edge of the keep
  bracket over a sealed take's waveform; release re-runs the model on the new span. The
  posteriorgrams are retained, `retranscribe()` costs ~1 ms, and a full re-run costs ~0.5 s
  per 30 s of audio (measured: 49.4 ms for 3.1746 s, single-threaded). **The first slice
  never has to be right**, which is the only thing that makes a one-click capture
  psychologically safe.
- **`amplitude` was doing three jobs and has been split.** `[built]`
  `Notes::Event::amplitude` is the mean note-posteriorgram value over a note's frames, a model
  probability, and it was also written straight into exported MIDI velocity and the preview
  synth, so Quarry's exported dynamics **were** the model's uncertainty. `Notes::Event` now
  carries `velocity` and `onsetConfidence` alongside it, and everything that wanted loudness
  reads `velocity`.

  One correction to the original specification, which said velocity was "RMS of the source
  audio over the note's span". That is right for a monophonic take and wrong for a polyphonic
  one, because the span's RMS contains every other note sounding at the same time and a quiet
  note held under a loud chord reads as loud. `Lib/Model/NoteVelocity.{h,cpp}` uses the CQT
  energy in the note's own fundamental and first two harmonics, peaked over the attack. See
  `ANALYSIS.md` §2.1.
- **Widen `okstudio/Transcribe.h` before adopting it.** Its pimpl returns only
  `vector<Note>` and throws the posteriorgrams away — and the posteriorgrams (contours
  264 × N, notes 88 × N, onsets 88 × N at 86.13 fps) *are* the description pillar. Add a
  `const Posteriorgrams& posteriorgrams() const` accessor. Every product on the line
  benefits; only Quarry needs it today.
- **Every estimator is arithmetic we write ourselves, in a `juce_core`-only header.** Not
  a preference — the entire classic C++ MIR stack is copyleft (Essentia AGPLv3, madmom's
  models CC BY-NC-SA, Chordino GPLv2+, libKeyFinder GPLv3, aubio GPL, QM-DSP GPL). Key,
  tempo, chroma, chords and section boundaries are about 800 lines of arithmetic over data
  already in RAM. Writing them is cheaper than the licence conversation, and it makes them
  unit-testable by a test exe linking `okstudio_kit` alone.
- **The four descriptions are ranked, and the ranking is a decision.** Owen picked all four
  when asked; that is a wish list, not a priority order, and nobody had ranked them. Ranked:
  *(1)* per-note confidence — already computed, nearly free, and the only one that makes the
  MIDI *better*; *(2)* tempo — not a description at all, a correctness bug fix; *(3)* key —
  cheap, and it auto-fills the snap dropdown that already exists; *(4)* chords, section
  labels and the paragraph — decoration for a man whose stated goal is MIDI into Simpler,
  shipped last and **gated on measured accuracy on 20 real captures Owen chose**. A chord
  chip that is wrong a third of the time is worse than no chord chip.
- **Tempo is a prerequisite, not a feature.** `TimeQuantizeOptions.h:24-26` defaults to
  120 BPM / 4-4 and only ever reads the DAW playhead. Quarry is standalone-first, so every
  `.mid` it exports before tempo estimation exists carries a fabricated tempo map and lands
  in Live at the wrong tempo — at exactly the moment the product is supposed to deliver.
  The estimate feeds `TimeQuantizeInfo::bpm` as the **standalone** source; the playhead stays
  the override when hosted.
- **No live musical readout.** Not deferred — refused. See *Roads not taken*.
- **Capture is standalone-only.** Loaded as a VST3 in Live with System Audio selected,
  Quarry would hear Live's master bus, which contains its own audition voice and whatever
  MIDI it is driving. The plugin build hard-disables the loopback source and shows a 34 px
  plate explaining why. The VST3 exists to *emit* MIDI, Strata's shape. *(Landed in the fork
  first, and with a second reason this section did not have: an exclusive-mode driver serves one
  client at a time, so a device the plugin took would be a device the host could lose. A hosted
  Quarry never constructs an `AudioDeviceManager` at all, hides the driver, input and channel
  pickers, and records the audio the host sends it.)*
- **Three threads, and the third one is new to the line.** Audio thread plays back and
  makes no decisions. Message thread does UI and the ~1 ms `retranscribe()` fast path.
  **Analysis thread** is a `juce::ThreadPoolJob` on a one-thread pool running the staged
  pipeline. Strata composes on the message thread because it costs 0.3 ms; a transcription
  plus a description costs seconds, so it cannot live there.
- **Cancellation is not optional.** A mouse-only user cannot press Ctrl+C. Every job checks
  `shouldExit()` between stages, and Cancel is a 140 × 34 button in the header that is
  present the entire time a job runs.

---

## The mouse-only interaction table

| Action | The gesture | Target |
|---|---|---|
| Keep what you just heard | **one left-click** on KEEP | 190 × 76, header, undeletable |
| Start / stop listening | one left-click, Listen/Pause chip | 72 × 24 on the 34 px SOURCE bar |
| Change the source | one left-click, `StepComboBox` | 220 × 24, popup rows 34 px |
| Bring in a file or video | **one drag** onto SOURCE content | full-width `FileDragAndDropTarget` |
| Change how much gets kept | **one drag** on the bracket edge | 34 × 96 grab bar |
| …click twin | one click, a duration chip | 5 × (64 × 34) |
| …fine twin | one click, `<` / `>` | 2 × (34 × 34), 1 s steps |
| Load an earlier take | one left-click on a take chip | 148 × 80 |
| Re-slice a sealed take | **one drag** on the bracket over its waveform | 34 × 96 |
| Cancel a running job | one left-click, Cancel | 140 × 34, header |
| Select an editing verb | one left-click, mode chip | 5 × (68 × 24) on a 34 px bar |
| Move a note | one drag on the **note body** | ≥ 34 px hit rect (see below) |
| Change a note's length | Length mode, horizontal drag on the body | ≥ 34 px hit rect |
| Change a note's velocity | Velocity mode, **relative** vertical drag, 2 px/unit | ~254 px full sweep |
| Delete notes | Erase mode, click or drag across bodies | ≥ 34 px hit rect |
| Fix an octave / semitone error | one left-click, `−8ve −1 +1 +8ve` | 4 × (40 × 24) on the bar |
| Select a time region | **one drag along the ruler** | 34 px tall, full width |
| Bulk-fix a region | one left-click, a bulk verb | ≥ 34 px, appears on selection |
| Undo | one left-click | 84 × 24, one gesture = one entry |
| Scroll pitch | wheel over the grid | click twin: Octave `<` `>` |
| Scroll time | wheel over the ruler | click twin: Bars `<` `>` |
| Find what to fix | one left-click, *Next uncertain bar* | 190 × 34, floating, always visible |
| Hide the guesses | one drag, Conf Floor rotary | 60 px + a 34 px stepper twin |
| Fix a wrong tempo | one left-click, `÷2` or `×2` | 44 × 24, permanently visible |
| Fix a wrong key | one left-click, relative-swap or runner-up | 34 px / the candidate plate |
| Fix a wrong chord | one left-click, a chord chip cycles | 72 × 34 |
| Rename a section | one click on the label, one click in the popup | 6 rows × 34 px = 204 px |
| Get it into Ableton | **one drag** from the header MIDI plate | 220 × 62, `DraggingHandCursor` |
| …or live | one left-click, Send-live toggle | 100 × 24 |
| Fold / unfold a section | one left-click on the bar's left end | ~92 px × 34 px `foldZone()` |
| Detach a section | one left-click, Detach | 90 × 24 |

No double-click. No modifier. No keyboard path anywhere — the fork's `NumericTextEditor`
and `QuarryMainView::keyPressed` (space, shift+space, shift+backspace, r, m, c) are
deleted rather than restyled. Right-click is an optional accelerator only, and every
right-click path has a left-click twin.

---

## The analysis pipeline, stage by stage

| # | Stage | Thread | Cost | Notes |
|---|---|---|---|---|
| 0 | WASAPI loopback poll | **capture thread** (kit-owned) | — | `AUDCLNT_STREAMFLAGS_LOOPBACK`, hand-written COM; JUCE 8.0.8 has no loopback |
| 1 | Ring write | **capture thread** | ~0 | Wait-free SPSC into a 300 s stereo ring at the endpoint's own rate. No allocation, no locks, no logging. Each block is stamped with its QPC timestamp — see the gap trap below |
| 2 | KEEP: reserve | **message thread** | < 1 ms | Atomically reserves the last N seconds of the ring against the writer and hands the region to the pool. **Does not copy.** |
| 3 | KEEP: copy + seal | **analysis thread** | ~10 ms | Copies the reserved region into a `KeepBuffer`, writes `source.wav`, releases the reservation, creates the take directory |
| 4 | Focus pre-filter *(optional)* | **analysis thread** | < 0.5 s | `FocusFilter.h` over `juce::dsp::FFT`: median-filtered HPSS, or a **true audio-domain** band-pass. Genuinely different from the existing min/max-note control, which only restricts the output loop *after* the CNN saw the full-band mix (`Notes.cpp:78-88`) |
| 5 | Resample + normalise | **analysis thread** | < 0.2 s | `resampleForModel` → mono 22050 Hz, Lagrange + IIR lowpass. Loudness-normalise here: the material is YouTube, and MAESTRO-trained expectations collapse on degraded audio |
| 6 | Model | **analysis thread** | ~0.47 s / 30 s | ONNX Runtime does the CQT + harmonic stacking; RTNeural does all four CNN heads. Single-threaded **by construction** (`SetIntraOpNumThreads(1)`, serial loop) — this does not improve with cores. Posteriorgrams retained |
| 7 | Notes + the three-way split | **analysis thread** | ~1 ms | `Notes::convert` with the kit's `setNoteFilter` clamping range *inside* convert. Then `confidence` / `onsetConfidence` / `velocity` split apart |
| 8 | Tempo + meter | **analysis thread** | < 0.1 s | `TempoEstimator.h` autocorrelates the summed **onset posteriorgram**, then phase-aligns to maximise onset energy on beats. Writes `TimeQuantizeInfo::bpm` |
| 9 | Key + ribbon | **analysis thread** | < 0.05 s | `KeyEstimator.h`: duration- **and confidence-**weighted 12-bin pitch-class histogram vs Temperley-Kostka-Payne (~85 % on symbolic input), then an 8-bar sliding window |

> **A cut-down version of row 9 landed in the fork first**, as `Lib/Model/KeyEstimate.h`. It is
> the histogram and nothing else: weighted by duration and measured velocity rather than by
> model confidence, correlated against Krumhansl-Kessler rather than Temperley-Kostka-Payne, one global
> answer with no ribbon and no runner-up, and it runs on the message thread because it is
> microseconds over note events that already exist. Two guards the design above does not mention
> turned out to be necessary: a correlation is positive for any histogram that is not perfectly
> flat, and a sparse one outscores a real scale, so the fork gates on how many pitch classes carry
> the take and on an absolute confidence floor. Anything built here should keep those and treat
> the shipped struct as a different type that happens to share a name.
| 10 | Chords *(gated)* | **analysis thread** | < 0.1 s | `ChordEstimator.h`: beat-synchronous chroma folded from **Quarry's own notes**, 25 templates (12 maj, 12 min, N.C.), small Viterbi smoothing |
| 11 | Sections *(gated)* | **analysis thread** | < 0.2 s | `SectionEstimator.h`: self-similarity matrix over beat chroma, checkerboard-kernel novelty peaks. **Boundaries only** |
| 12 | Describe (local) | **analysis thread** | < 1 ms | `Describe::local()` composes the paragraph from `AnalysisReport` by string formatting. **Always runs** |
| 14 | Publish | **message thread** via `callAsync` | — | Each stage publishes as it finishes, so key and tempo chips populate while chords still run |
| 15 | Audition | **audio thread** | — | Reads an immutable POD note window through a `NoteFeed` (Strata's `ScoreFeed` shape: three fixed slots, two atomics, memcpy then release-store). Strata's `processBlock` order exactly, including that the outgoing MIDI copy is **last** |

**The gap trap, and it is the one unverified assumption under the whole capture model.**
WASAPI loopback does not necessarily deliver packets while the endpoint is idle; silence can
arrive as a *gap* rather than as zero blocks. If that happens, "KEEP LAST 30s" returns 30
seconds of *audio* spanning several minutes of *wall time* — a splice of unrelated material
that will look like a transcription bug. Every ring block is therefore stamped with its QPC
timestamp, the reader checks for discontinuity, and a gap is **zero-padded to real time**
rather than closed up. This is tested against a synthetic writer in the kit, not
code-reviewed.

Nothing in stages 4–13 may touch the audio thread, allocate on it, or lock it. Audio never
crosses `NoteFeed` — its slots are POD by value, and a slot holding stem audio is not a slot.

---

## How a wrong note gets fixed

This is the part of the product that decides whether it is usable, because a 3-minute
YouTube capture through a 16,782-parameter instrument-agnostic CNN on out-of-distribution
material yields *hundreds* of wrong notes. Note-at-a-time correction is a job, not an
interaction. Four mechanisms, in the order they should be reached for.

**1. Find them without hunting.** The per-bar confidence ribbon paints the whole take, and
**Next uncertain bar** (190 × 34, floating over the grid, always visible) scrolls and selects
the next bar whose 10th-percentile note confidence is below the floor. One click, repeat.
No scrubbing, no keyboard, no zooming out to look.

**2. Delete the guesses in bulk.** The **Conf Floor** rotary fades every note below the
threshold to 25 % alpha — they stay visible, so you can see what is being discarded, which is
AnthemScore's threshold done as one drag. Beside it, **Delete faded**, one click, one undo
entry. On a dense mix this is the single highest-value operation in the product: it takes the
fix rate from notes-per-minute to bars-per-click.

**3. Fix a region.** A **drag along the 34 px ruler** selects a time span — a big, safe,
edge-anchored target, which is why the ruler is 34 px and not 24. A selection puts a row of
bulk verbs on the roll bar, each ≥ 34 px: **Transpose ±8ve**, **Transpose ±1**,
**Quantise**, **Delete**, **Delete faded**. Every verb is one click and one undo entry.

**4. Fix one note.** Pick a mode chip, then drag the note **body**. There are no edge handles
anywhere in Quarry. Two rules make this work at real zoom levels:

- **Minimum hit width, decoupled from drawn length.** A note is *drawn* at its true length
  but its hit rectangle is at least 34 px wide, centred on the body; overlaps resolve
  front-most, then highest-confidence. Without this the whole editing surface fails its own
  contract: at a 30-second view across ~1300 px of canvas the scale is ~43 px/s, so a note at
  the default 75 ms minimum duration draws **3 px wide**, and getting it to a 34 px target
  means zooming to ~4.8 s visible — two bars at 120 BPM, at which point you cannot see a
  phrase. Lattice gets away with `minColW = 36` because its steps are quantised; a
  transcription is free-time and the time-axis gate has to be written from scratch.
- **Pitch and time are never coupled in one gesture.** basic-pitch's signature failure is an
  octave or semitone slip. Dragging a body freely in two axes inside a 36 px row fixes the
  pitch and introduces a timing error you then fix in a second mode. So the pitch nudge row
  (`−8ve  −1  +1  +8ve`, 40 × 24, permanently on the ROLL bar) is the pitch path, it applies
  to the selection or to the last-touched note, and Move mode's drag is time-only by default.

**Undo is one click and one gesture is one entry.** `pushUndo()` fires once at `mouseDown`
before the first write, never per note. 16-deep ring in the processor; the button greys
itself. Erase mode is the one genuinely destructive mode, and a mis-click there can take out
a run of overlapping notes — undo is the mitigation, and the active mode is echoed at the
cursor in the hint strip so the mode is never invisible.

---

## Data model and on-disk formats

### `src/QuarryNotes.h` — pure header, `juce_core` only, unit-tested

Replaces Lattice's quantised `Cell{Empty,Start,Hold}` grid, which does **not** survive the
port: a transcription is free onsets, arbitrary lengths, overlapping polyphony, 88 chromatic
rows. `cellIndex`, `MAX_STEPS` and the fixed-array-of-atomics publish scheme all go.

```cpp
struct Note {
    double        startSec, lenSec;
    int           pitch;            // 21..108
    juce::uint8   velocity;         // harmonic-band CQT energy at the attack, not span RMS
    float         confidence;       // mean note posteriorgram
    float         onsetConfidence;  // peak onset posteriorgram at onset
    juce::uint8   focusId;          // 0 full mix, 1 harmonic, 2 percussive, 3..5 bands
    juce::uint8   origin;           // 0 model, 1 hand-edited, 2 video scanline
};
struct NoteSet { std::vector<Note> notes; };
```

Keeps Lattice's **file shape** (pure header, `juce_core` only, test-exe linkable) and its
**function names** — `noteAtPoint`, `noteStartCovering`, `noteLengthAt`, `collectNotes`,
`pushUndo` — so the canvas port is mechanical rather than a rewrite.

### `src/analysis/AnalysisReport.h` — pure header, `juce_core` only

```cpp
struct AnalysisReport {
    // Note: the fork already ships a KeyEstimate (Lib/Model/KeyEstimate.h) carrying only
    // rootNote, isMinor and confidence. This one is a richer type of the same name.
    KeyEstimate               global;      // winner, runnerUp, relative, confidence
    std::vector<KeyEstimate>  ribbon;      // 8-bar sliding window
    TempoEstimate             tempo;       // bpm, confidence, half/double alternatives
    Meter                     meter;
    std::vector<ChordSpan>    chords;
    std::vector<SectionSpan>  sections;    // boundaries + neutral label + guess
    ConfidenceSummary         conf;        // histogram, per-bar 10th percentile
    juce::String              headline, paragraph;
};
```

One rendering: `Describe::local()` formats it into English with string formatting. There was to
have been a second, an opt-in Claude call posting the same struct as JSON, and it is cut.

### On disk

```
%APPDATA%/OK Studio/Quarry/Takes/<uuid>/
    source.wav          host-rate stereo, verbatim, gap-padded
    model.wav           mono 22050, what the CNN actually saw
    thumb.bin           AudioThumbnail cache
    take.quarrytake     ValueTree via the kit's StateHelpers.h
```

`take.quarrytake` holds: the note set, the `AnalysisReport`, the focus mode, the keep window
and its wall-clock stamp, the `Source::id()` it came from, and the edit history depth.
**Reopening Quarry reopens the last take with its transcription intact** — which is what
makes a 40-second analysis affordable, because you never pay it twice.

Layout state — folds, detach flags, detached window frames — lives in
`QuarryProcessor::LayoutState` on the **session ValueTree, never as parameters**. None of it
changes a note, and exposing it to host automation only adds ways to break a session.

### Parameters

Renamed **once**, now, `versionHint 1`, then append-only forever. `src/QuarryParams.h`:
`NOTE_SENSITIVITY`, `SPLIT_SENSITIVITY`, `MIN_NOTE_DURATION`, `MIN_NOTE`, `MAX_NOTE`,
`PITCH_BEND_MODE` (No / Single / **MPE** — the engine already computes multi-bend and the UI
currently bins it), `KEY_ROOT`, `KEY_TYPE`, `KEY_SNAP`, `TIME_DIVISION`, `QUANTIZE_FORCE`,
`AUDITION_MIX`, `SOURCE_GAIN`, `MUTE`, `CONFIDENCE_FLOOR`, `FOCUS_MODE`, `TEMPO_MULTIPLIER`,
`MIDI_CHANNEL_MODE`, `KEEP_SECONDS`. Non-automatable ValueTree keys move
from `NnId.h` to `src/QuarryId.h`. This is the last free chance.

---

## Licence constraints and what they forbid

Written down here because a future contributor will find the permissive-looking downstream
tag first and act on it. Mirrored into `docs/MODEL_LICENSES.md`, per weight artefact, with
its upstream repo and its training data — cheap at two models, archaeology at six.

**Forbidden outright, in any repackaging:**

- **Demucs / HTDemucs weights.** Alexandre Défossez, verbatim, in
  `facebookresearch/demucs#327`: *"The model weights are not covered by the MIT license, and
  are provided only for scientific purposes."* `StemSplit/demucs-onnx` and
  `Intel/demucs-openvino` both relabel the converted weights MIT; **neither has the authority
  to grant that.** Not a grey area.
- **Open-Unmix UMXL** — CC BY-NC-SA 4.0. Easy to grab the better checkpoint by accident.
- **ADTOF** — CC BY-NC-SA across the *whole repository*, code and models, no carve-out.
- **MT3 checkpoints** — no stated licence at all. Absence of a licence is not permission.
- **Essentia** — AGPLv3, meaning publishing Quarry's source to anyone who receives the
  binary; its pretrained models are CC BY-NC-**ND**, so they cannot even be fine-tuned. UPF's
  commercial licence explicitly does not sublicense Essentia's own dependencies.
- **madmom's pretrained models.** The BSD source is the trap — a licence scanner says fine.
  Every beat/downbeat/chord tracker needs a `.pkl`, and those are CC BY-NC-SA with an explicit
  contact-the-author clause. The BSD DBN *algorithm* classes are usable; the models are not.
- **Chordino/NNLS-Chroma (GPLv2+), libKeyFinder (GPLv3), aubio (GPL), QM-DSP (GPL)** and
  every Vamp plugin built on them.
- **FFmpeg**, bundled. Its own LGPL checklist requires *removing any prohibition of reverse
  engineering from the EULA* — a decision about the whole product line — and hosting exactly
  corresponding source next to every download, forever. **OpenCV is the same trap wearing an
  Apache-2.0 badge**: its Windows decode path is a prebuilt LGPL `opencv_videoio_ffmpeg*.dll`.
- **Melodyne-style polyphonic editing of the audio.** Celemony's DNA is patented. Quarry's
  editable artefact is MIDI, which is both the safer position and the better product, and
  Quarry should not be described in terms that invite the comparison.

**Resolved 2026-08-17** (see `docs/PLAN.md` → *Licence decisions required*): the vendored
**ASIO SDK** is dropped rather than licensed, so Steinberg's agreement is not needed here, though
the kit's `AudioCapture` keeps ASIO and keeps the question. **JUCE** needs no purchase yet: this
tree is on JUCE 8, which has no splash screen to disable and licenses by revenue instead, free
under Starter to $20k/yr. `spotify/basic-pitch` does ship a NOTICE, and its contents are now
reproduced in `NOTICE`. What remains before anything is sold is the basic-pitch **weights**
question, which needs counsel, not a decision.

**Apache-2.0 §4(b) is now partly satisfied in the fork** and must be satisfied in the new repo
on day one. A root `NOTICE` landed on 2026-07-30 naming Ronssin, Tibor Vass and Spotify's
basic-pitch, stating the changes at the product level, and listing the third-party components
in the tree; what is still missing is the per-file statement of changes. Every ported file **keeps** its `Created by Damien Ronssin`
header (stripping it breaches §4(c)) and gains `// Modified 2026 by Owen Kent for Quarry.`;
`LICENSE` ships. §6 grants no trademark rights, which is why `NeuralNoteVideo` does not survive
as a product name.

**The single point of failure nobody is protecting.** The prebuilt `onnxruntime.lib` is
2.91 GB on disk and comes from a ~100 MB tarball published **once**, in March 2023, on one
individual's personal GitHub release (`tiborvass/libonnxruntime-neuralnote v1.14.1-neuralnote.1`).
It is gitignored, so nothing local preserves it. If that release disappears, nothing in the
OK Studio line that transcribes will ever build again. **Mirror the 100 MB tarball to storage
Owen controls and point `OKSTUDIO_ONNXRUNTIME_CACHE` at a stable directory outside the build
tree.** Twenty minutes. Do it first. *(The often-repeated "2.9 GB download" is wrong and has
been getting this deferred: the download is 100 MB, the expanded static lib is 2.9 GB.)*

**Two things that build is, that constrain everything downstream.** It is a
`--minimal_build --include_ops_by_config --enable_reduced_operator_type_support` build against
basic-pitch's operator set, so it **physically cannot load any other ONNX model**; and it is
`--disable_exceptions`, so a failed `Ort::Session` construction **aborts the process** rather
than throwing (`Features.cpp:16` has no error path). It also forces `/MT` on the whole binary,
so every dependency Quarry ever adds must be static-CRT or you get a wall of LNK2038 with no
cause named, and LTO must stay off (`C1047`). Any future model is a runtime rebuild with a
union op config, or a sidecar process. **Plan for sidecar.**

---

## What it deliberately is not

- **Not an MPC.** No drum pads, no slice-and-play, no chop-a-break-into-16. "Sampling
  machine" means sampling audio **into** the machine. An earlier design pass made exactly
  this mistake.
- **Not a live transcriber.** Notes will never appear while Owen listens. The CQT front end
  needs over a second of audio before its low bins mean anything and note events resolve
  backwards from the end. Any UI implying otherwise is a lie.
- **Not a stem separator.** It ships harmonic / percussive / band focus filters under those
  names. If Owen wants true instrument stems, that requires either a licensed vendor model or
  one he obtains himself, and the UI says so rather than implying HPSS is separation.
- **Not a drum transcriber.** basic-pitch transcribes pitched notes and commonly returns
  nothing on drums. Live already does three-class drum conversion. Quarry does not compete.
- **Not a general multi-instrument transcriber.** 2026 SOTA on that task is ~0.60 onset F1
  at 60M–1.3B parameters with autoregressive decoding. Wrong shape, wrong size, wrong bet.
- **Not a sheet-music reader.** Audiveris is AGPL; oemer is Python plus deep models. Closed.
- **Not always-on disk recording.** RAM ring only, and the cost is stated in the UI.
- **Not a network product.** Nothing leaves the machine. The optional description call was the
  one exception and it is **cut**, so the local paragraph is not the fallback, it is the whole
  feature. Quarry opens no socket.

---

## Roads not taken

*Kept so they are not reopened from scratch. `Strata/docs/SC.md` is the precedent.*

**The live scrolling analysis strip** (the "Listener" proposal's differentiator). Rejected
on both accuracy and arithmetic. Its own DSP could not work as specified: a "4-tap polyphase
decimate to 12 kHz" is an integer-ratio operation and 44.1 kHz — the commonest endpoint rate
— is not an integer multiple of 12 kHz, so every live BPM would read ~8.8 % wrong and every
chroma bin would be mistuned by ~1.5 semitones. And a 2048-point FFT at 12 kHz gives 5.86 Hz
bins, where one semitone at C2 is 3.9 Hz, so the two octaves where a chord's root actually
lives are unresolvable. Even fixed, live tempo carries ~20 % octave errors, live key from FFT
chroma is weaker than the note-derived version, and live chords sit under a ~84 % ceiling —
scrolling continuously in the user's peripheral vision, styled "provisional" to apologise for
being wrong. A UI band-aid on a data problem, and it costs bar height the roll needs. **What
survives from that proposal is everything that made it good: KEEP, the RAM ring, retroactive
re-slice, the `AudioCapture::listen()` split, the nothing-touches-disk consent story, and the
habit of gating a feature on measured accuracy.** The live *level meter* and elapsed-time
readout stay; they are cheap and honest.

**Armed Record / Stop / Analyse.** Three clicks bracketed by two moments of timing, and the
timing is the failure mode. Rejected in favour of KEEP. Its only advantages — no always-on
thread, no ring, ~130 px more roll — are not worth requiring prescience.

**Renovating the fork in place.** 2.9 GB of `ThirdParty`, two git submodules, C++17,
a `GLOB_RECURSE` build, and a main view that is hardcoded absolute pixel coordinates painted
over a background PNG (`QuarryMainView::resized()`) with a fixed window size. There is
no incremental path from that to Keys' Section system. Anyone estimating "just restyle it" is
wrong by an order of magnitude. *(The fork stays checked out as the donor and as the day-one
spike vehicle — that is not the same as renovating it.)*

**Path B of `VIDEO_ENHANCEMENT_PROPOSAL.md`** — visual transcription of piano *performance*
video as "ground truth". The document claims *"Direct physical ground truth"* and
*"Accuracy Gain: Very High"*; the published numbers say the opposite. The best visual-only
system (ViT, IJCAI 2025) scores onset F1 **0.68** on real YouTube top-down piano video where
the audio-only baseline scores **0.877** on the same clips — and that 0.68 is measured against
pseudo-labels generated *from the audio*, so the comparison is already generous. Video buys
pitch accuracy, never timing: 30 fps quantises onsets to ±16 ms against basic-pitch's 11.6 ms
hop. Building it would make transcriptions *worse*. **The proposal now carries a correction
banner and that section is marked closed.** The Synthesia path is a different thing entirely —
de-rendering a MIDI file, not perception — and it is kept.

**Path A2, sheet-music OMR.** Closed as a no: Audiveris is AGPL, oemer is Python plus deep
models.

**A dedicated DESCRIBE section, and a per-note-confidence lane.** Both cost a bar plus
content — roughly 86 px of grid — and the docked roll's 12-row floor does not have it. The
paragraph expands inside ANALYSIS; confidence is the note body's own shading plus an 8 px
heat ribbon.

**An interactive per-bar confidence strip.** At 90 bars across 1400 px each cell is 15 px,
under the floor on the axis that matters. Demoted to a non-interactive ribbon; *Next uncertain
bar* is the click target.

**Detaching sections by default.** Launching into two or three overlapping OS windows makes
window management the user's first task, and window management with one mouse is the most
expensive thing you can ask for. Everything docks; the roll gets ≥ 12 rows at the minimum
window size; detach exists and is never required.

**Capture inside the VST3.** It would hear Live's master bus, including Quarry's own audition
voice, and a device the plugin opened for itself is a device the host can lose. Loopback
is hard-disabled in the plugin build. *(Already true in the fork.)*

**An extended chord vocabulary, a single confident key label, asserted section names.**
Chord estimation's ceiling is ~84 % on root/majmin and trained human annotators disagree by
over 15 %, so "Cmaj9#11" is a promise that cannot be kept. Key always shows the runner-up and
the relative sibling. Sections assert boundaries and *offer* functional labels.

---

## Open questions

1. **Describe scope.** Chords, section labels and the paragraph are ranked last and cost
   ~2 weeks. Cut all three, keep chords only, or build all four? *(Recommend: chords only,
   gated on a 20-capture accuracy measurement; sections as boundaries with neutral labels;
   local paragraph only.)* Decides parameter IDs, which are append-only forever.
2. ~~**The Claude description, and whose key pays.**~~ ✅ **Decided 2026-08-17: local only, and
   the online read is cut outright.** A baked-in key is extractable and unbounded, a key file is
   a chooser almost nobody completes, and a proxy is a service with a standing bill. `M9` is
   0 days now. `DESCRIBE_ONLINE` is struck from the parameter list before the ids freeze.
3. ~~**ASIO.**~~ ✅ **Decided 2026-08-17: dropped.** `JUCE_ASIO=1` and `ThirdParty/ASIO/` are
   gone. WASAPI and DirectSound cover both sources Owen actually picked, and Quarry records
   loopback, so ASIO's round-trip latency was never being spent. The kit keeps its own copy of
   this question.
4. ~~**JUCE licence.**~~ ✅ **Answered 2026-08-17: the question was built on JUCE 7.** JUCE 8
   has no splash screen at all, so there is no flag to set and no splash to ship. It licenses by
   revenue: Starter is free and allows closed-source commercial distribution to $20k/yr, Indie
   is $800 perpetual to $300k. Nothing to buy or decide at repo creation.
5. **The 20 Synthesia URLs.** The C++ scanline port cannot be scoped without them. If 3D
   renderers or heavy particle effects dominate Owen's sample, the spec changes materially.
6. **Unverified, must be settled by measurement, not by argument:** does WASAPI loopback gap
   when the endpoint is idle on this machine (decides whether the ring's time axis is real);
   does `Lattice/src/ui/RollCanvas.{h,cpp}` extract from `LatticeProcessor&` (15 methods over
   37 call sites, all step-grid shaped — this is the largest single estimate risk in the plan);
   is `Notes::convert` linear in input length (the melodia trick sorts a frames × 88 index
   vector; the measured timings cover only the two stages that *are* linear).
