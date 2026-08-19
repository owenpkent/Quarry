# Quarry — The Analysis Engine

Companion to `DESIGN.md` and `PLAN.md`. This document covers only the audio-to-MIDI engine:
what it is today, what is measurably wrong with it, what has replaced it in the field, and
what to build. Everything about the window, the piano roll and the sampler lives elsewhere.

Getting the notes right is this document. Everything else the take can be asked about (the
statistics, the key and chord reading, the acoustic measurements, and the material profiles
that decide which of them run) is `STATS.md`.

---

## 1. What the engine is today

Quarry's transcription is Spotify's Basic Pitch (2022), inherited from NeuralNote. `Lib/` was
byte-identical to upstream apart from a short-input guard and an LTO flag until the §2 fixes; the
model, the features and the CNNs still are, and the decoder's upstream behaviour is still reachable
and still tested.

The pipeline:

| Stage | Where | What it does |
| --- | --- | --- |
| Downmix and resample | `SourceAudioManager.cpp:285-295` | Everything becomes mono at 22.05 kHz |
| Harmonic-stacked CQT | `Lib/Model/Features.cpp` | One ONNX session, whole buffer in one call, output `frames × 264 × 8` |
| Four CNNs | `Lib/Model/BasicPitchCNN.h:88-118` | RTNeural, frame by frame, ~17k parameters total |
| Three posteriorgrams | | contour `264`, note `88`, onset `88`, at 86.13 fps |
| Threshold derivation | `Lib/Model/BasicPitch.cpp` | Noise floor and Otsu split, per take, before decoding |
| Note decoding | `Lib/Model/Notes.cpp` | Threshold, local-max onset picking, energy timeout, melodia trick |
| Velocity | `Lib/Model/NoteVelocity.cpp` | Harmonic-band CQT energy at the attack, per note |
| Scale snap and range | `Lib/MidiPostProcessing/NoteOptions.cpp` | Optional, purely symbolic |
| Time quantize | `Quarry/Source/TimeQuantizeOptions.cpp` | Optional, against a tempo it mostly guesses |

Two numbers frame everything below. The model is **about 17,000 parameters**, and the frame
rate is **86.13 fps, so 11.6 ms per frame**. The first is the quality ceiling. The second is
the timing floor. Neither moves without replacing the model.

Basic Pitch's own published accuracy is roughly **52% note-level F1 on vocals** and **79% on
GuitarSet**, and it degrades sharply on anything denser than a single instrument. That is not
a criticism of the port, which is faithful. It is the model.

**Where that ceiling went, 2026-08-18.** The model did get replaced, out of process: the
sidecar of §4.2 is built, integrated behind `QUARRY_SIDECAR_CMD`, and measured through
Quarry's own binary at onset F1 0.98, velocity F1 0.96+, and pedal F1 0.83 to 0.90 on real
piano recordings, against this tier's 0.775. Everything in §2 stands as the record of the
CPU tier, which remains the shippable Apache-2.0 default and the automatic fallback. The
measured story of the replacement is the end of §4.0 and §4.2, and `SIDECAR.md` is how to
run it.

---

## 2. What is wrong, worst first

Each of these is a specific defect with a specific fix. They are ordered by how much they
hurt a real take, not by how hard they are.

> Every line reference in §2 describes the code **as it was**, because that is what the defect
> descriptions are about, and following one now lands somewhere else. The table below says where
> each fix went. The diagnoses are kept in full rather than deleted: they are the argument for
> why the current code looks the way it does, and the next person to touch the decoder needs it.

**Status.** §2.1 to §2.7 are built, along with two defects not listed below: the 125 ms minimum
note duration, and a stereo file being transcribed from its left channel alone. §2.8 is untouched
by design: it is what the GPU tier is for.

| | Defect | What was built |
| --- | --- | --- |
| 2.1 | Velocity is confidence | `Lib/Model/NoteVelocity.{h,cpp}`, a tier-independent stage |
| 2.2 | Onsets quantised to 11.6 ms | Parabolic vertex on the onset peak, `Notes::_subFrameOffset` |
| 2.3 | Offsets are a fixed timeout | Release relative to the note's own peak, past the core span |
| 2.4 | Semitone clusters impossible | Neighbour suppression conditional on relative energy |
| 2.5 | Divide by zero makes notes from silence | Guarded, falls back to the raw onsets |
| 2.6 | Every knob turn re-sorts 36 MB | Sorted once per take; order proven invariant |
| 2.7 | Thresholds handed over raw | Both derived per take; knobs offset from the derivation |
| — | 125 ms minimum note duration | 75 ms, swept on the bench |
| — | Stereo dropped file used channel 0 | Downmixed, as the capture path always did |

**Measured**, on `tools/bench` against the synthetic corpus, current against the pre-fix engine:

| | legacy | current |
| --- | --- | --- |
| Note onset F1 | 0.709 | **0.923** |
| Precision / recall | 0.693 / 0.726 | 0.857 / **1.000** |
| Onset and offset F1 | 0.500 | 0.560 |
| Onset and velocity F1 | 0.372 | **0.780** |
| Mean onset error | 5.4 ms | **3.7 ms** |

The per-case numbers are where the argument actually is, because each case was built to isolate
one defect: a trill goes 0.474 to 1.000, minor seconds 0.667 to 1.000, a semiquaver run 0.800 to
1.000. Sub-frame refinement is invisible in F1 at a 50 ms tolerance, as it must be, and shows up
only in the onset error column.

Three caveats worth carrying forward.

The corpus is synthetic, so these are an upper bound and the ordering matters more than the
values. Nothing here has met a real piano.

`sustained` is the one case that is still bad, at F1 0.571 with 15 notes detected against 6, and
it was bad before the fixes too. Long overlapping notes are the remaining CPU-tier weakness and
none of §2 addresses them.

Every change to the decoder is gated behind a parameter defaulting to upstream's behaviour, so
the ten parity cases in `Tests/notes_test.h` still check this port against basic-pitch's Python
implementation. Those passing is not evidence the new path is good; it is evidence the old path
is intact, which is what makes `--legacy` on the bench mean anything.

### 2.1 Velocity is not velocity

`MidiFileWriter.cpp:35` and `SynthController.cpp:42` pass `Notes::Event::amplitude` as MIDI
velocity. `amplitude` is the mean note-posteriorgram value over the note's frames
(`Notes.cpp:118-131`), which is a model probability. **Every `.mid` Quarry has ever exported
encodes the model's confidence as the performance's dynamics.**

`DESIGN.md:265-272` already names this and specifies the fix: split into `confidence`,
`onsetConfidence` and `velocity`. Built as described, in `Lib/Model/NoteVelocity.{h,cpp}`.

One correction to that specification. It defines `velocity` as "RMS of the source audio over
the note's span". That is correct for a monophonic take and wrong for a polyphonic one,
because the span's RMS contains every other note sounding at the same time: a quiet note held
under a loud chord reads as loud. Use instead the CQT magnitude in the bins belonging to that
pitch's fundamental and first two or three harmonics, summed over a short window from the
onset, in dB, normalised across the take. The CQT is already computed and already in RAM, so
this costs a loop, and it is right in both cases.

It also matters beyond export: `KeyEstimate.cpp:102` weights the pitch-class histogram by
`amplitude`, so key detection is currently weighted by confidence too.

**And this is not a Basic Pitch patch.** MuScriptor emits no velocity at all (§3), so the GPU
tier needs exactly this computation and can inherit nothing from the model. Build it as a
stage that takes note events plus audio and returns velocity, sitting downstream of whichever
model produced the notes, and both tiers get their dynamics from one implementation. Written
as a patch to `Notes.cpp` it would have to be written twice, and the second time against a
model that offers nothing to patch.

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
and still is: the fix below made the threshold relative without exposing the frame count.

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

### 2.9 There is no pedal, on any tier

Named late, in the world-class review of 2026-08-18, and it belongs in §2 because it is the
largest single absence for the primary use case: no stage in Quarry emits or models CC64.
Half of a piano score is the pedal lane, and without damper state the offset the decoder
hunts for does not acoustically exist: the pedal decides where a note ends, not the key.
`sustained` at F1 0.571 with 15 notes against 6 is the closest the synthetic corpus can get
to this shape, and §2.3's relative release cannot fix it, because no decoder rule recovers a
state the model never saw. The fix is a model that regresses pedal (§3.0.2), CC64 through
`MidiFileWriter`, and a pedal column on the bench. Until then every exported piano take is
missing half its information, silently.

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
segments of **16 kHz mono**, mel at 100 fps. Released in three sizes: **103M (CPU-suitable),
307M (default), 1.4B**.

Two different F1 numbers circulate and this document previously conflated them. The released
large reports Multi-F1 **48.2 against YourMT3+'s 21.9**. The paper's own scaling table is
lower, Multi-F1 **35.2 at 60M rising to 40.5 at 1.3B**, because it is the pre-RL ablation.
Quote them separately or not at all.

Install is a normal Python package: `pip install muscriptor`, or on Windows with a GPU
`uvx --torch-backend=cu128 muscriptor serve`. HuggingFace authentication and licence
acceptance happen before the weights download, which puts the CC BY-NC acceptance on the user,
which is precisely where §3.1 wants it. There is a `serve` mode with a local web UI, so an
HTTP endpoint probably already exists and the line-based JSON protocol in §4.2 may not need
writing at all.

#### 3.0.1 What it does not do

Verified against the paper rather than the coverage, because the coverage is wrong about the
first of these.

- **No key, chord or tempo output.** The press says it detects all three; the paper is
  note-level only, emitting pitch, onset, offset and instrument. Everything in `STATS.md` §4
  and §5 stays Quarry's own work on either tier.

- **No velocity.** "The tokenization scheme does not encode velocity or dynamics." A larger
  model therefore does not fix §2.1, it makes it *permanent*: there is no amplitude anywhere in
  the output, neither to misuse as Basic Pitch does nor to correct. Dynamics have to come from
  the audio on both tiers, which is why §2.1 is now specified as a tier-independent stage.

- **No two notes of the same pitch and instrument at once.** A pedalled piano re-strike is
  exactly this shape: strike, hold the pedal, strike the same key again while the first is
  still ringing. The paper measures what excluding those notes is worth: Onset F1 falls
  **60.4 to 51.8** and Offset F1 **49.0 to 41.9** once overlapping same-pitch notes are
  counted. Solo piano is the intended use case here and it sits on the model's stated weak
  point, so §4.2's spike has to test pedalled material specifically rather than take the
  headline number on trust.

#### 3.0.2 The piano specialists, which are the piano answer

Revised 2026-08-18. The paragraph that stood here treated the specialists as a footnote for
"narrower scopes". That had the emphasis backwards: the narrower scope is the primary use
case, and §3.0.1's three limits all land on it. The specialists erase all three.

- **Kong et al. (ByteDance, 2021)**, the same work §2.2 cites: a CRNN regressing continuous
  onset and offset times, onset F1 **96.72** on MAESTRO, and it emits **velocity and sustain
  pedal** as first-class outputs. Ships as `pip install piano-transcription-inference`. Code
  Apache-2.0; the weights are MAESTRO-trained, which is the same unsettled exposure the
  basic-pitch weights already carry, and the sidecar keeps them out of the binary either way.
  It is also non-autoregressive, so an ONNX export is a bounded project rather than a
  research one, unlike MuScriptor.
- **hFT-Transformer** (Sony, 2023): stronger still on note-with-offset, heavier to run,
  licence to verify at integration.
- **Transkun**: event-based semi-CRF decoding, stated MIT for code and weights, which would
  make it the licence-cleanest of the three; verify before relying on it.

So the tier picture inverts: **the piano flagship should be a specialist, and MuScriptor is
the mixes tier**, not the other way round. Pointing the generalist at solo pedalled piano
optimises for the material Quarry cares least about at the expense of the material it cares
most about.

For monophonic material **SwiftF0** and **CREPE-Notes** remain the stronger options, and
**Demucs** as a separation front end (§4.3) is what lets mixes be transcribed one stem at a
time instead of all at once.

### 3.1 The licensing finding, which decides the architecture

| Component | Code | Weights |
| --- | --- | --- |
| Basic Pitch | Apache-2.0 | Apache-2.0 |
| MuScriptor | MIT | **CC BY-NC 4.0, non-commercial only** |
| YourMT3+ | **GPL-3.0** | not clearly stated |
| Kong et al. piano | Apache-2.0 | MAESTRO-trained; same unsettled exposure as basic-pitch |
| Transkun | stated MIT | stated MIT; verify at integration |

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

**Built**, in `tools/bench`, and it earned its keep immediately: the first run said the §2 fixes
were a net regression, and it was right. The adaptive offset as first written let a short blip
ring down to a low floor until it was long enough to pass the minimum-length test, which cost 84
spurious notes on a 60-note corpus. Reasoning had not caught that and was not going to.

```
cmake -S . -B build -DQUARRY_BUILD_BENCH=ON
py tools/bench/make_corpus.py Tests/bench_corpus
build/tools/bench/Bench_artefacts/Release/Bench.exe Tests/bench_corpus --baseline tools/bench/baseline.tsv
```

- Pairs of `<name>.wav` and `<name>.mid` in one directory. `make_corpus.py` writes a synthetic
  piano corpus with exact ground truth so the tool is runnable before anyone downloads MAESTRO.
- Note-level precision, recall and F1 at the standard 50 ms onset tolerance, three ways: onset
  only, onset and offset, onset and velocity. Matching is maximum bipartite, as mir_eval does it,
  because greedy matching under-reports recall in exactly the dense passages this exists to test.
- Mean onset error in milliseconds, which is the only column that can see §2.2 at all.
- `--legacy` runs the pre-fix engine, so a change is attributable to the fixes rather than to the
  corpus. `--baseline` exits non-zero when aggregate onset F1 falls.

Two lessons from building it, both about the instrument rather than the engine. The first corpus
rang every note for `duration * 2.5 + 0.5` seconds, so the "fast run" case contained no short
notes and the sweep it existed to inform was meaningless. And a corpus whose shortest note is
longer than the floor being swept can only ever argue for raising the floor, which is why the
trill case exists.

**Still missing: real material, and there is a rung between synthetic and real.** The additive
corpus cannot represent pedalled playing, which is the stated weak point of both tiers. The next
rung is **rendered MAESTRO**: the MIDI-only release (57 MB, CC BY-NC-SA, fine for the bench under
the personal-use posture in `PLAN.md`), test split, excerpts stratified by pedal density, rendered
through a sampled piano. Real performances, real velocities, real pedal, exact ground truth, still
no real microphones. Ground-truth offsets follow the standard pedal-extension convention (a note's
reference offset is the later of key release and pedal release), and onset F1 stays the primary
number until the engine emits pedal. The top rung is MAESTRO's audio proper, 120 GB, when it is
downloaded. A one-time `mir_eval` cross-check of the bench's arithmetic comes with the first real
corpus, so the numbers are comparable to the literature rather than merely to each other. Tooling:
`tools/bench/fetch_maestro.py` and `tools/bench/make_real_corpus.py`, corpus in
`Tests/bench_corpus_maestro/`.

**Measured, 2026-08-18.** Built and run the same day, plus three corpora this paragraph did not
plan: MAESTRO's actual recordings (the full 101 GB archive is local now, and
`Tests/bench_corpus_maestro_real/` holds real-audio twins of the same 22 windows), SMD (50 real
Disklavier recordings, Zenodo), BabySlakh (multi-instrument mixes with per-stem ground truth),
and a 24-case corpus rendered from the local GM MIDI collection. The `mir_eval` cross-check
passed exactly on onsets, per case and aggregate. The current engine, onset F1:

| corpus | material | onset F1 |
| --- | --- | --- |
| synthetic (old bench) | additive piano | 0.923 |
| rendered MAESTRO | real performances, sampled piano | 0.824 |
| MAESTRO real audio | real performances, real recording | 0.775 |
| SMD | real recordings, different piano and room | 0.760 |
| GM collection | mixed classes, FluidR3 render | 0.552 |
| BabySlakh | multi-instrument mixes | 0.335 |

Two structural findings. Pedal density predicts the damage: on MAESTRO material the low-pedal
bucket holds near 0.93 while the high-pedal bucket falls toward 0.6-0.7, all of it recall
(precision stays around 0.9, so the engine misses notes under sustain rather than inventing
them), which is §2.9 with a number on it. And rendering bias is real but runs the wrong way for
comfort: the current engine scores 0.049 *better* on the FluidSynth render than on the real
recording, while MAESTRO-trained specialists score better on the real audio, so a rendered
corpus understates the gap to the specialists.

### 4.1 CPU tier: fix Basic Pitch

In this order, cheapest and safest first:

1. ~~The NaN guard (§2.5).~~ Done.
2. ~~Remove the per-tweak sort (§2.6).~~ Done. Behaviour-preserving, and the argument for why is in
   §2.6 so it can be reviewed rather than trusted.
3. ~~Split `amplitude` (§2.1)~~ Done, with velocity from harmonic-band CQT energy
   rather than span RMS. This is the most visible fix in the list.
4. ~~Sub-frame onset refinement by parabolic interpolation (§2.2).~~ Done: 5.4 ms to 3.7 ms.
5. ~~Threshold auto-calibration from the take's own statistics (§2.7).~~ Done.
6. ~~Conditional neighbour suppression (§2.4), behind a parameter.~~ Done: minor seconds 0.667 to 1.000.
7. ~~Relative rather than absolute offset threshold (§2.3).~~ Done. `energyThreshold` is still not exposed.

Items 3 and 6 would have changed the golden test data, so both are gated behind a parameter that
defaults to upstream's behaviour and the fixtures still pass unchanged.

### 4.2 GPU tier: a sidecar, not in-process inference

**Run the large model in a separate process.** Quarry writes the take to a wav, talks to the
sidecar, and gets back note events that enter through the existing `Notes::Event` path so the
piano roll, post-processing and export all work unchanged. `muscriptor serve` may mean the
protocol is already HTTP and does not need designing.

**It is not a complete answer, and §3.0.1 is why.** The sidecar addresses missed and invented
notes, which is the larger of the two complaints, and it addresses nothing else. Velocity has
to be computed from the audio either way (§2.1), key and chords remain Quarry's (`STATS.md`),
and the same-pitch limitation lands on pedalled piano specifically. Read this section as "the
notes get much better", not as "the transcription problem is solved".

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

**The part that may already be handled.** MuScriptor transcribes 5-second segments, so long
takes need overlapping windows stitched with de-duplication across the seams, and seam
handling is where transcription quality quietly degrades. This document previously called that
the hard part. It may not be ours to solve: the CLI takes a whole file, and the paper credits
instrument conditioning with keeping transcriptions coherent across segment boundaries. So the
package probably chunks internally. Running it on a three-minute take answers this in minutes
and is part of the spike below, rather than a design problem to reason about in advance.

**Bake it off before integrating any of it.** Revised 2026-08-18: the spike as first written
auditioned one model by ear. It is now a three-way bake-off scored by the bench on the rendered
MAESTRO corpus of §4.0, plus an electronic track and a full mix: **Basic Pitch against Kong et
al.'s piano specialist against MuScriptor** (§3.0.2). That settles which model leads on which
material with a number, how each handles a long file, whether the same-pitch limitation is
audible on real pedalling, and what `serve` exposes. MuScriptor's HuggingFace licence acceptance
is Owen's and blocks only its own lane; the other two lanes run without it. Tooling in
`tools/bakeoff/`. Every other decision in this section depends on the answer.

**Run, 2026-08-18, and the answer is decisive.** Onset F1, identical corpora, identical
`mir_eval` scoring, all on CPU:

| engine | rendered MAESTRO | MAESTRO real | SMD real | BabySlakh mixes |
| --- | --- | --- | --- | --- |
| Quarry (Basic Pitch + §2) | 0.824 | 0.775 | 0.760 | 0.335 |
| Kong et al. | 0.935 | 0.982 | 0.944 | 0.381 |
| Transkun | **0.977** | **0.985** | **0.953** | 0.394 |
| MuScriptor medium | not run | 0.849 | 0.822 | **0.433** |

Findings, in order of consequence:

- **The piano gap is 21 points on real recordings** (0.775 against 0.985), and the specialists
  hold roughly 0.95-0.98 in the high-pedal bucket where the current engine falls toward 0.6.
  §3.0.2's inversion is confirmed: on piano, the specialist is the flagship.
- **No piano model survives a mix.** On BabySlakh the specialists manage 0.38-0.39 against
  Quarry's 0.335, despite scoring 0.98 on solo piano. The material class dominates the model,
  so per-material routing plus §4.3's separation is mandatory, not an optimisation.
- **Offsets under pedal are a convention question, not a quality ranking.** Against
  pedal-extended ground truth on MAESTRO real audio, Kong's onset+offset F1 is 0.712 while
  Transkun's collapses to 0.02-0.06 on high-pedal cases at 0.97+ onset F1: Transkun reports
  key-release offsets, Kong sustain-extends natively and also emits CC64, pedal and velocity.
  The sidecar adapter resolves this either way; until it does, Kong is the complete piano
  answer and Transkun (stated MIT, code and weights) is the accuracy and licensing answer.
- **The GPU tier needs no GPU.** Kong runs a 30 s excerpt in about 7 s and Transkun in about
  11 s on CPU alone, both faster than real time. The sidecar is viable on any machine and a
  GPU only makes it instant.
- **MuScriptor confirms the routing split from the other side.** Licence accepted, medium
  checkpoint fetched, run on the 5090 (about 6-10 s per 30 s excerpt with cu128 torch; note
  the cu128 wheel index tops out at torch 2.11, and 2.13+cpu does not count as older than
  2.11+cu128 for pip, so the CUDA build must be forced explicitly). It is the **best engine
  on mixes** (0.433 BabySlakh, 0.673 on the GM corpus against Quarry's 0.552) and clearly
  behind the specialists on piano (0.849 real MAESTRO, 0.822 SMD). It beats the current
  engine on every corpus. Even so, 0.43 on mixes says separation stays on the plan; the
  generalist narrows the mix gap, it does not close it.
- **Two adapter findings the sidecar must inherit.** MuScriptor's MIDI writer moves the whole
  transcription onto a detected bar grid (`BeatGrid.onset_delay`), a DAW-drop convention
  that landed every onset a constant +1.0 s late here and collapsed scored F1 to 0.108 until
  `detect_tempo=False` disabled it; wall-clock times must come from that flag, with the bar
  alignment reapplied by Quarry's own writer if wanted. And `TranscriptionModel.load_model`
  defaults were CPU: the adapter selects CUDA explicitly.
- **The sidecar itself is built and measured, same day.** `tools/sidecar/` (protocol v1,
  newline JSON over stdio, three engines, lazy model cache, pedal and velocity in the
  response) plus `Lib/Sidecar/SidecarClient` (native pipes: `juce::ChildProcess` cannot
  write a child's stdin, verified against the JUCE source) plus `--sidecar` on the bench.
  Through Quarry's own binary: kong 0.981 / 0.944 and transkun 0.985 / 0.953 onset F1 on
  real MAESTRO / SMD, identical to the Python bake-off to the third decimal, so the pipe is
  lossless. The column the bake-off could not see: **onset+velocity F1 0.958 (kong) and
  0.977 (transkun)** against the CPU tier's 0.275, at 3.4 to 3.9 ms mean onset error. Both
  stated complaints, missed and invented notes and no dynamics, are answered by this one
  path.
- **§5 step 3 is complete, same day.** The plugin transcribes through the sidecar when
  `QUARRY_SIDECAR_CMD` is set (`QUARRY_SIDECAR_ENGINE` picks the engine), fed the
  original-rate audio, with BasicPitch as automatic per-take fallback and the decoder-only
  sensitivity knobs skipping re-decode on sidecar takes. Sidecar-measured velocities bypass
  `NoteVelocity`; a velocity-less engine (MuScriptor) still flows through it. CC64 reaches
  exported MIDI through `MidiFileWriter`, and the bench grew a `pedal` column: span-level
  F1 at the 200 ms tolerance Kong et al. use, against ground truth that now carries the
  window's CC64 stream including mid-pedal window opens. Measured through the full pipe:
  kong pedal F1 **0.827** on real MAESTRO and **0.861** on SMD. And a correction recorded
  the day it was made: transkun does emit CC64, sparsely; a single-file check saw none, the
  corpus run scored its sparse toggles at **0.895**. Both piano lanes carry pedal.
- **Separation, sized (§4.3, §5 step 5), same day.** Demucs (htdemucs, personal tier per
  §3.1) rides the sidecar as a `sep+` engine prefix: separate, drop the drums stem,
  transcribe the rest per stem, merge with stem-tagged instruments; about 1.4 s to separate
  a 30 s mix on the 5090. Measured: sep+muscriptor **0.457** on BabySlakh against 0.433
  without, **0.684** on the GM corpus against 0.673, and sep+kong 0.393, worse than the
  generalist alone. The recovery is +0.01 to +0.02, not the leap §4.3 hoped for. Two
  caveats before that hardens: BabySlakh is 16 kHz mono renders, a domain htdemucs was not
  trained on, and the bass stem still goes to the generalist rather than the monophonic
  tracker `STATS.md` §7 specifies. Mixes remain the open front, which is what the AMT
  Challenge numbers in §3 already said of the whole field.

### 4.3 Separation, if the bench says mixes are the gap

Demucs as an optional pre-stage inside the same sidecar, transcribing per stem. This is the
largest single win available for full-mix input and it is cheap once §4.2 exists. Revised
2026-08-18: it no longer waits on a measurement to justify its existence, because full mixes
are already the documented failure mode; the bench's job is to size the recovery, not to
relitigate the gap. Routing is per stem (`STATS.md` §7): bass to a monophonic sub tracker,
drums to onset and kick-spec extraction, the remainder to the polyphonic model. Personal tier
only, exactly as §3.1 already decided.

---

## 5. Sequencing

Revised 2026-08-18; the 2026-08-17 revision moved the sidecar ahead of the CPU fixes, and its
reasoning is kept in `PLAN.md`'s status history. What changed today: the CPU fix list is done,
the bench exists, and the world-class review added four things the previous order did not know
about. The real corpus outranks the spike, because a model decision gets read off a number,
not vibes. The spike is a bake-off with a piano specialist in it (§3.0.2). Pedal is in scope
(§2.9). And separation for EDM no longer waits on a measurement to justify its existence, only
to size it.

1. ~~**The real corpus** (§4.0).~~ Done 2026-08-18, and it outgrew the spec: six corpora,
   including MAESTRO's real audio and SMD's real recordings. The measured table closes §4.0.
2. ~~**The bake-off** (§4.2).~~ Done, same day. Transkun and Kong take piano, MuScriptor
   takes mixes; the verdict table and its findings close §4.2.
3. ~~**The sidecar proper.**~~ Done, same day, through to the plugin: service, client,
   `QUARRY_SIDECAR_CMD`, CC64 into exported MIDI, and a pedal column on the bench. See the
   §4.2 measured block and `SIDECAR.md`.
4. **Velocity calibration**, halved in scope by measurement: the sidecar engines' own
   velocities score 0.958 and 0.977, so calibration now matters only for the CPU tier's
   `NoteVelocity` stage (0.227 on real MAESTRO), which is the fallback path. Still worth
   doing against MAESTRO's Disklavier velocities; no longer on the critical path.
5. ~~**Separation** (§4.3).~~ Done and sized, same day: +0.01 to +0.02 as built, caveats in
   §4.2. The mixes front stays open.
6. **CPU decoder work: stop.** §2.1 to §2.7 are done and everything left sits on the
   17k-parameter ceiling, which now has a working replacement beside it.

After steps 1 to 5, the program is `STATS.md`'s: tempo, the key bench, the library and batch
answer, the acoustic chain. Plus the mixes front, which is the one measured disappointment:
a bass-stem monophonic tracker, and separation retested on 44.1 kHz stereo material rather
than 16 kHz mono renders.
