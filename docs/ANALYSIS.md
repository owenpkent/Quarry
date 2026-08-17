# Quarry — The Analysis Engine

Companion to `DESIGN.md` and `PLAN.md`. This document covers only the audio-to-MIDI engine:
what it is today, what is measurably wrong with it, what has replaced it in the field, and
what to build. Everything about the window, the piano roll and the sampler lives elsewhere.

---

## 1. What the engine is today

Quarry's transcription is Spotify's Basic Pitch (2022), inherited unchanged from NeuralNote.
`Lib/` is byte-identical to upstream apart from a short-input guard and an LTO flag.

The pipeline:

| Stage | Where | What it does |
| --- | --- | --- |
| Downmix and resample | `SourceAudioManager.cpp:283` | Everything becomes mono at 22.05 kHz |
| Harmonic-stacked CQT | `Lib/Model/Features.cpp` | One ONNX session, whole buffer in one call, output `frames × 264 × 8` |
| Four CNNs | `Lib/Model/BasicPitchCNN.h:88-118` | RTNeural, frame by frame, ~17k parameters total |
| Three posteriorgrams | | contour `264`, note `88`, onset `88`, at 86.13 fps |
| Note decoding | `Lib/Model/Notes.cpp` | Threshold, local-max onset picking, energy timeout, melodia trick |
| Scale snap and range | `Lib/MidiPostProcessing/NoteOptions.cpp` | Optional, purely symbolic |
| Time quantize | `Quarry/Source/TimeQuantizeOptions.cpp` | Optional, against a tempo it mostly guesses |

Two numbers frame everything below. The model is **about 17,000 parameters**, and the frame
rate is **86.13 fps, so 11.6 ms per frame**. The first is the quality ceiling. The second is
the timing floor. Neither moves without replacing the model.

Basic Pitch's own published accuracy is roughly **52% note-level F1 on vocals** and **79% on
GuitarSet**, and it degrades sharply on anything denser than a single instrument. That is not
a criticism of the port, which is faithful. It is the model.

---

## 2. What is wrong, worst first

Each of these is a specific defect with a specific fix. They are ordered by how much they
hurt a real take, not by how hard they are.

### 2.1 Velocity is not velocity

`MidiFileWriter.cpp:35` and `SynthController.cpp:42` pass `Notes::Event::amplitude` as MIDI
velocity. `amplitude` is the mean note-posteriorgram value over the note's frames
(`Notes.cpp:118-131`), which is a model probability. **Every `.mid` Quarry has ever exported
encodes the model's confidence as the performance's dynamics.**

`DESIGN.md:265-272` already names this and specifies the fix: split into `confidence`,
`onsetConfidence` and `velocity`. It is still unbuilt.

One correction to that specification. It defines `velocity` as "RMS of the source audio over
the note's span". That is correct for a monophonic take and wrong for a polyphonic one,
because the span's RMS contains every other note sounding at the same time: a quiet note held
under a loud chord reads as loud. Use instead the CQT magnitude in the bins belonging to that
pitch's fundamental and first two or three harmonics, summed over a short window from the
onset, in dB, normalised across the take. The CQT is already computed and already in RAM, so
this costs a loop, and it is right in both cases.

It also matters beyond export: `KeyEstimate.cpp:102` weights the pitch-class histogram by
`amplitude`, so key detection is currently weighted by confidence too.

### 2.2 Onset timing is quantised to 11.6 ms and never refined

Onsets are picked as local maxima on the frame grid (`Notes.cpp:87-95`) and converted with
`_modelFrameToTime`, which is integer frames times hop over sample rate. There is no
sub-frame refinement anywhere.

This is exactly the limitation Kong et al. removed by regressing precise onset and offset
times instead of reading them off the frame grid, taking onset F1 on MAESTRO from **94.80%
(Onsets and Frames) to 96.72%**. Their approach needs a model with a regression head, so it
belongs to the GPU tier. But a large part of the benefit is available for free: fit a
parabola through the onset posteriorgram at the peak frame and its two neighbours and take
the vertex. That is three multiplies per onset and it dissolves the audible grid.

### 2.3 Note offsets are a fixed timeout

A note ends when its energy has been below the frame threshold for `energyThreshold = 11`
frames, about 128 ms (`Notes.cpp:102-111`). That constant is hardcoded in the struct default
and never exposed.

There is no decay model, no release, no pedal. A piano note under sustain and a bowed note
held to the bar line are both cut by the same absolute rule against a global threshold, so
anything that decays gets truncated early and anything with a noisy tail runs long. The
threshold should be relative to that note's own sustain level, not to a global constant.

### 2.4 The decoder cannot represent a semitone cluster

Both decoding passes zero the neighbouring pitch bins across the note's entire duration:
`Notes.cpp:123-128` in the main pass, and the `inhibit` lambda at `Notes.cpp:174-179` in the
melodia trick. The intent is to suppress spectral leakage into adjacent bins, which is real.

The effect is that a genuine minor second cannot survive decoding. Clusters, close voicings
and any two-part writing that touches a semitone lose a voice, structurally, every time.

The fix is to make the suppression conditional rather than unconditional: zero a neighbour
only when its energy is below some fraction of the centre bin's. Two strong adjacent bins are
two notes; one strong and one weak is leakage. Keep the current behaviour behind a parameter
so takes that relied on it are not disturbed.

### 2.5 A divide by zero that produces notes out of silence

`Notes.h:244` computes `inferred = max_onset * inferred / max_min_notes_diff`.
`max_min_notes_diff` is zero on any take where the note posteriorgram never rises, which is
any near-silent or heavily gated input. The result is NaN.

NaN then fails every comparison at `Notes.cpp:95`, including the two that exist to reject the
frame, so the gate that should reject an onset passes it. Quiet input can therefore produce
spurious notes rather than none. Guard it: if `max_min_notes_diff <= 0`, use the raw onsets
unchanged.

### 2.6 Every knob turn re-sorts the entire file

`updateMIDI` re-runs the whole decoder, and `Notes.cpp:145` sorts `mRemainingEnergyIndex`,
which holds one record per `(frame, pitch)` pair. Five minutes of audio is 25,800 frames, so
**2.27 million records, about 36 MB, re-sorted on every parameter change.** This is why the
sensitivity knobs feel sluggish on long takes.

The sort is removable outright, and provably so. After decoding, every entry of
`mRemainingEnergy` is either zero or its original `inNotesPG` value: the only writes are
`Notes.cpp:121` and the `inhibit` lambda, and both assign zero. So the descending order of
the non-zero entries is identical to the descending order of the raw posteriorgram, which
does not change between parameter tweaks. Sort once when the audio arrives, then skip zeros
while iterating. Same output, no sort per tweak.

### 2.7 The two most important parameters are handed to the user raw

`BasicPitch.cpp:26-27` maps the sensitivity knobs to thresholds by `1 - x`, with no reference
to the material. Published work on Basic Pitch finds that moving the threshold from 0.5 to
0.6 alone was worth close to **50% relative F1** on one corpus. That is a larger effect than
most of the fixes above, and Quarry currently asks the user to find it by ear with no
feedback.

Derive the default from the take's own posteriorgram distribution, for example a percentile
or an Otsu split of the onset posteriorgram, and let the knobs offset from that starting
point rather than set an absolute value.

### 2.8 Structural limits that no post-processing reaches

Mono, 22.05 kHz, 88 pitch bins, one generic model, no instrument conditioning, no separation.
Full mixes are the documented failure mode of Basic Pitch, and none of §2.1 to §2.7 changes
that. That is what the GPU tier is for.

---

## 3. What has replaced it

The field has moved a long way since 2022.

**The 2025 AMT Challenge** benchmarked multi-instrument transcription on 76 newly composed
pieces across eight instruments. MT3 as the baseline scored **0.3932** F1. The winner, MIROS
(MusicFM encoder, about 370M parameters, T5-style decoder), scored **0.5998**. YourMT3+ MoE
scored **0.5938**. Only two of twenty-one registered teams beat the baseline at all.

Polyphony remains the wall, and the numbers are blunt about it. Going from one instrument to
three, MIROS falls from **0.7193 to 0.4367** and YourMT3+ from **0.7594 to 0.3918**. Common
failures are instrument leakage and hallucinated instruments. Nobody has solved dense mixes.

**MuScriptor** (Kyutai and Mirelo, July 2026) is the most practical option. Decoder-only
transformer over mel-spectrograms, autoregressively emitting MIDI-like tokens from 5-second
segments of 16 kHz mono. Released in three sizes: **103M (CPU-suitable), 307M (default),
1.4B**. On its own test set it reports Multi-F1 **47.8 to 48.2 against YourMT3+'s 21.9**. It
installs as a normal Python package with a CLI and a Python API.

For narrower scopes there are stronger options still: **hFT-Transformer** (Sony) for piano,
**SwiftF0** and **CREPE-Notes** for monophonic material, and **Demucs** as a separation front
end so that mixes get transcribed one stem at a time instead of all at once.

### 3.1 The licensing finding, which decides the architecture

| Component | Code | Weights |
| --- | --- | --- |
| Basic Pitch | Apache-2.0 | Apache-2.0 |
| MuScriptor | MIT | **CC BY-NC 4.0, non-commercial only** |
| YourMT3+ | **GPL-3.0** | not clearly stated |

This is the same wall `DESIGN.md:278-283` already hit with the classic C++ MIR stack, where
Essentia, madmom, Chordino, libKeyFinder, aubio and QM-DSP are all copyleft or non-commercial.
The conclusion there was to write the arithmetic rather than have the licence conversation.
Here that is not an option, because the thing being licensed is 300M trained parameters.

So the consequence is architectural, not just legal:

- **The CPU tier is the shippable product.** Basic Pitch is Apache-2.0 and stays, which is
  the whole reason §2 is worth doing rather than being made moot by §4.
- **The GPU tier is personal and non-commercial as designed.** That is fine for the stated
  audience today and it is not fine for a paid product later. Keeping it out of process, as
  an optional component the user installs and accepts the licence for themselves, is what
  keeps that door open instead of closing it quietly.

---

## 4. What to build

### 4.0 The bench, first, before anything else

"Not good enough" cannot be fixed without a number, and Quarry does not currently have one.
`PLAN.md` notes that `Tests/test_data/note_events.output.json` is stale and fails on upstream
NeuralNote too, so the one existing check is not ground truth either.

Build a small evaluation harness in `tools/`:

- Audio paired with reference MIDI. Start with public material (GuitarSet, MAESTRO excerpts,
  a vocal set) and add real captures chosen by hand.
- Note-level precision, recall and F1 at the standard 50 ms onset tolerance, reported three
  ways: onset only, onset and offset, and onset with velocity.
- One command, one table, baseline committed.

This is the step that converts "it's not good enough" into something trackable, and it is
also the only honest way to decide whether §4.2 earns its complexity. Everything below is
written assuming it exists.

### 4.1 CPU tier: fix Basic Pitch

In this order, cheapest and safest first:

1. The NaN guard (§2.5). Two lines.
2. Remove the per-tweak sort (§2.6). Behaviour-preserving, and the argument for why is in
   §2.6 so it can be reviewed rather than trusted.
3. Split `amplitude` into three fields (§2.1), with velocity from harmonic-band CQT energy
   rather than span RMS. This is the most visible fix in the list.
4. Sub-frame onset refinement by parabolic interpolation (§2.2).
5. Threshold auto-calibration from the take's own statistics (§2.7).
6. Conditional neighbour suppression (§2.4), behind a parameter.
7. Relative rather than absolute offset threshold (§2.3), and expose `energyThreshold`.

Items 3 and 6 change the golden test data, so 4.0 has to be real before they land.

### 4.2 GPU tier: a sidecar, not in-process inference

**Run the large model in a separate process.** Quarry writes the take to a wav, talks to the
sidecar over a line-based JSON protocol, and gets back note events that enter through the
existing `Notes::Event` path so the piano roll, post-processing and export all work unchanged.

Four independent reasons, any one of which would be sufficient:

- **It works this week.** MuScriptor ships as a Python package with a CLI. Exporting an
  autoregressive decoder to ONNX is a project with an uncertain outcome; running it as a
  subprocess is an afternoon.
- **A CUDA fault does not take the DAW with it.** In-process inference in a VST3 puts a GPU
  context inside the host. Out of process, a crash is a failed job.
- **It is the licence boundary.** An arms-length separate process the user installs
  themselves keeps CC BY-NC weights and GPL code out of Quarry's binary and out of Quarry's
  distribution.
- **It sidesteps the Blackwell problem entirely.** Stock `onnxruntime-gpu` ships no sm_120
  kernels, so on a 5090 the CUDA provider silently falls back to CPU. PyTorch with cu128
  supports sm_120 today, and MuScriptor's Windows install already asks for exactly that
  backend. The 4070 is sm_89 and unaffected either way.

**Model choice.** `muscriptor-medium` (307M) as the default: it is the paper's own default
and fits the 4070's 12 GB with room to spare, which makes the 4070 the design target and the
5090 headroom rather than a requirement. `muscriptor-large` (1.4B) as the option when the
5090 is present. `muscriptor-small` (103M) is CPU-suitable and should be measured on the
bench against Basic Pitch out of interest, but Apache-2.0 is why Basic Pitch stays the
shipping default regardless of how that comes out.

**The part that will actually be hard.** MuScriptor transcribes 5-second segments. Long takes
need overlapping windows stitched together with note de-duplication across the seams, and
seam handling is where transcription quality quietly degrades. It needs its own bench case.

### 4.3 Separation, if the bench says mixes are the gap

Demucs as an optional pre-stage inside the same sidecar, transcribing per stem. This is the
largest single win available for full-mix input and it is cheap once §4.2 exists. It is last
because it should be justified by a measurement rather than by intuition.

---

## 5. Sequencing

1. **The bench** (§4.0). Nothing else is measurable without it.
2. **CPU fixes** (§4.1), in the listed order. Each one re-run against the bench.
3. **Sidecar and MuScriptor** (§4.2), measured on the same bench as the CPU tier so the two
   tiers are comparable in one table.
4. **Demucs** (§4.3), if and only if step 3's numbers say mixes are where the loss is.
