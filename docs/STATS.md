# Quarry — The Report

Companion to `ANALYSIS.md`, which covers getting the notes right. This document covers
everything else a take can be asked about: the statistics, the key and chord reading, the
acoustic measurements, and the material profiles that decide which of them run.

Two use cases drive it and they are not the same job. **Transcribing piano** wants a score:
the notes, the pedal, the rubato, the hands. **Reverse-engineering electronic music** barely
wants notes at all. It wants the tempo to three decimal places, the bass root per bar, the
filter curve, the sidechain shape and the stereo picture per band. A single default serves
neither well, which is what §2 is about.

---

## 1. Two chains, and why the split is structural

Quarry has exactly one audio front end today: `SourceAudioManager.cpp:283` downmixes to mono
and resamples to 22.05 kHz, and everything downstream reads that. Nyquist is therefore
**11.025 kHz** and there is no stereo field at all.

That is correct for the model and useless for half of what follows. Anything about the top
octave, the stereo image, the master bus or the source's provenance is not merely degraded on
the model path, it is *absent*. Two chains, then:

| Chain | Source | Rate | Serves |
| --- | --- | --- | --- |
| **Musical** | `model.wav` via the CQT and posteriorgrams | mono 22.05 kHz | notes, key, chords, tempo, sections |
| **Acoustic** | `source.wav`, verbatim | host rate, stereo | spectrum, loudness, stereo, sidechain, provenance |

A third target joins these if the GPU tier lands: MuScriptor wants **mono 16 kHz**
(`ANALYSIS.md` §3), which is neither of the above. It is one more resample of audio already in
memory, not a third capture path, but the code should treat "what the model saw" as a property
of the tier rather than as a constant.

The musical chain exists. The acoustic chain does not, and it needs its own STFT over the
native-rate stereo audio.

That audio is already there, which is the good news and is easy to miss. `SourceAudioManager`
runs **two** writers (`SourceAudioManager.cpp:179-193`): one at the record rate in the record
channel count, and a second mono one at 22.05 kHz for the model. The stereo file is what the
acoustic chain wants and it costs nothing to produce. Two caveats: it is 16-bit, which is fine
for everything in §6 and not for noise-floor work; and it goes onto `mFilesToDelete`
(`SourceAudioManager.cpp:175`), so it is a temp file, not the durable take directory of
`DESIGN.md:499`. The acoustic chain must either run before cleanup or the file has to be kept.

There is a second consequence of the downmix worth naming, because it is an EDM problem
specifically: a wide stereo supersaw partially cancels when summed to mono, so the model sees
less of it than you hear. The acoustic chain is where that gets measured, and a per-band
mid/side reading is what tells you it happened.

---

## 2. Material profiles, not presets

### 2.1 What a profile is

A preset implies recalled knob positions. That is the wrong shape here, because `ANALYSIS.md`
§2.7 already commits to deriving thresholds from the take's own posteriorgram distribution. A
profile that sets absolutes would overwrite the measurement with a guess.

So a profile is a **prior the data can move**: it nudges the derived value and it decides
which analyses run and what the report shows. The UI must display both numbers, the derived
one and the profile's offset, or a bad transcription and a wrong profile become
indistinguishable, which is the failure mode that makes presets worse than no presets.

### 2.2 What each profile actually changes

The two use cases pull three decoder parameters in opposite directions. This is the argument
that profiles are load-bearing rather than convenience:

| | Piano | Electronic |
| --- | --- | --- |
| Neighbour suppression (`ANALYSIS.md` §2.4) | Conditional. Close voicings and semitone clusters are the material. | Aggressive. Detuned unison smears into neighbour bins and every smear is a phantom note. |
| Offset rule (§2.3) | Relative to the note's own decay, plus pedal. A fixed 128 ms timeout truncates every sustained chord. | Near-absolute is fine. Synth envelopes actually stop. |
| Tempo and quantise | Often meaningless. Rubato is the performance. Quantise off, report a tempo curve. | Hard grid, usually integer or half-integer BPM. Snap it, lock it, quantise on. |
| Note range | 21 to 108, and the model's low end is the weak part. | Sub-bass below the model's useful range wants a monophonic estimator, not Basic Pitch. |
| Report emphasis | Pedal, hands, timing deviation, dynamics. | Spectrum, sidechain, stereo, sound design (§6). |

Two more profiles fall out and cost almost nothing once the frame exists: **Solo /
monophonic** (one voice, so any second simultaneous note is an error and can be pruned
outright) and **Full mix** (the documented Basic Pitch failure mode, and the one that should
route to the sidecar and separation of `ANALYSIS.md` §4.2 to §4.3 rather than pretend).

### 2.3 The profile should be proposed, not asked for cold

Onset-timing regularity separates piano from electronic almost perfectly: machine-tight grid
versus rubato. Add mean polyphony and mean harmonicity and the four profiles above are
separable from statistics the take already produces. Measure, propose, let the user override
with one click. Asking cold is asking the user to answer a question the take has already
answered.

---

## 3. What can be measured, by what it costs

Ordered by cost, because the first three groups are loops over data already resident in RAM.
`BasicPitch.h:53-56` keeps all three posteriorgrams as member state and only clears them on
reset; the harmonic-stacked CQT persists in `Features::mOutput`. None of §3.1 to §3.3 needs
new DSP.

### 3.1 Free, from the note events

Note count, notes per second over time, mean and maximum simultaneous voices, pitch range,
interval histogram, note-duration histogram, legato versus staccato ratio (overlap between
consecutive notes), repeated-note rate.

### 3.2 Free, from the onset posteriorgram

Used for decoding today and then ignored. It carries: tempo and beat phase, downbeat, **swing
ratio** (histogram of onsets against the 8th grid; a peak at 58 to 62 % is swing), and
**timing deviation**, the mean absolute distance from the nearest grid line.

That last number is the most interesting single statistic for transcription work. It is the
rubato, expressed in milliseconds: it tells you how much of what you are hearing is the notes
and how much is the pushing and pulling. It is also the discriminator in §2.3.

### 3.3 Free, from the CQT

Per-note velocity from harmonic-band energy (`ANALYSIS.md` §2.1, **built**, in
`Lib/Model/NoteVelocity.{h,cpp}`), spectral
centroid over time, spectral flatness (which distinguishes a pitched note from a drum hit that
the model has hallucinated a pitch onto), band energy over time as a rough arrangement map,
and per-note inharmonicity.

### 3.4 The acoustic chain, which needs the new front end

Off `source.wav` at host rate in stereo: integrated LUFS, short-term loudness curve, true
peak, crest factor overall and per band, stereo correlation and mid/side balance per band,
clipping, DC offset, spectral tilt.

Plus one that is cheap and unusually valuable: **provenance**. A hard shelf in the average
spectrum at 16 kHz is a 128 kbps MP3; one at 19 to 20 kHz is a higher-bitrate transcode. If
the source is a lossy re-encode, everything downstream is worse and the user should be told
once rather than left to wonder why the transcription is poor.

### 3.5 The one to build first: tuning offset

The CQT is 3 bins per semitone (264 bins over 88 semitones, so 33.3 cents per bin), and
parabolic interpolation on peak positions across the take resolves the fractional offset well
under 10 cents.

Build it first because it is a *correction*, not a readout. YouTube uploads pitched a few
percent to evade Content ID, old recordings, and anything not at A440 are currently silently
wrong through the entire chain: pitch assignment, key detection, and playback. At around 50
cents the pitch assignment flips wholesale. Nothing downstream is trustworthy without it, and
it costs one pass over data already in memory.

---

## 4. Key

### 4.1 What ships today

`Lib/Model/KeyEstimate.cpp`: a 12-bin pitch-class histogram weighted by duration times measured
velocity (`KeyEstimate.cpp:103`), correlated against Krumhansl-Kessler probe-tone profiles
for all 24 rotations, best wins. Two guards that matter and should survive any rewrite: at
least 3 pitch classes must each carry 1 % of the take, and `kMinConfidence = 0.5f`
(`KeyEstimate.h:40`). Both exist because a correlation is positive for any histogram that is
not perfectly flat, and a sparse histogram outscores a real scale. A kick and a snare alone
score 0.84 against C minor.

### 4.2 What is wrong with it

1. ~~**The weight is confidence, not loudness.**~~ **Fixed.** The histogram was weighted by
   `amplitude`, the mean note posteriorgram, so it was weighted by how sure the model was.
   `ANALYSIS.md` §2.1 landed and key detection took the fix for free: the weight is now
   duration times measured velocity. It has **not** been re-measured against a labelled set,
   because there is no key bench, only the behavioural cases in `Tests/key_estimate_test.h`.

2. **Krumhansl-Kessler is the weakest of the standard profiles.** `DESIGN.md:361` already
   specifies Temperley-Kostka-Payne instead. Albrecht-Shanahan is worth benching too, being
   corpus-derived rather than probe-tone-derived.

3. **None of the classical profiles fit electronic music.** They encode functional tonality
   with a leading tone. A modal EDM track with a flat seventh and no leading tone correlates
   about equally with its relative major, and often wins on the wrong one. This is where the
   modal entries in `ScaleModes.h` (§5.2) earn their place: a per-mode profile set, chosen or
   reported alongside the major/minor answer, is a better model of the material than pretending
   everything is Ionian or Aeolian.

4. **Relative major and minor cannot be told apart from the histogram, ever.** The pitch-class
   content is identical by construction. `DESIGN.md:187` concedes this with the relative-swap
   chip, which is honest but is a manual fix for something partly solvable. Two things
   disambiguate and neither is in the histogram: metrical position (what lands on downbeats,
   what the take starts and ends on) and **the bass**. A pitch-class histogram built only from
   the lowest sounding note per beat identifies the tonic far more reliably than the full
   histogram, and in electronic music, where the bass line *is* the harmony, it is close to
   decisive. It needs tempo first, so it sequences after §3.2.

5. **One global answer hides modulation.** The 8-bar ribbon in `DESIGN.md:187` fixes this and
   has a second benefit: it makes the confidence number mean something. A take that modulates
   should report low global confidence *for a good reason*, distinguishable from a take that
   is simply ambiguous.

---

## 5. Chords, and the prior art next door

### 5.1 What the design specifies

`DESIGN.md:361` row 10: beat-synchronous chroma folded from Quarry's own notes, 25 templates
(12 major, 12 minor, N.C.), small Viterbi smoothing. The transition model is unspecified.

Worth stating because the coverage of MuScriptor claims otherwise: the GPU tier does **not**
hand us chords, key or tempo. `ANALYSIS.md` §3.0.1 has the check against the paper, which is
note-level only. Everything in §4 and §5 here is Quarry's own work on either tier, so none of
it is at risk of being made redundant by a better transcription model.

### 5.2 `../Keys` already contains most of it

Keys is the generative sibling: it builds chords rather than reading them. Its theory layer is
pure logic with no UI, unit-tested, and it maps onto Quarry's analysis need almost directly.

| File | What it is | What it gives Quarry |
| --- | --- | --- |
| `src/Chords.h` | 19 chord templates and a scored matcher: `2·covered − extras − 1.5·essentialMissing − 0.25·optionalMissing`, root mandatory, 3rd and 5th omissible cheaply | Strictly richer than 25 major/minor templates. Produces `Cm7`, `Gsus4`, `C5` rather than "C major". The power-chord template alone matters for EDM. |
| `src/ScaleModes.h` | Seven modes with **per-degree chord quality**, not just interval membership | A diatonic prior. Once the key is known, chords that fit the detected mode score higher. Cheap, and it removes a lot of errors. Also the source of §4.2 item 3. |
| `src/ChordMarkov.h` + `src/MarkovData.h` | A bigram transition table built from 88 hand-authored progressions (30 major, 30 minor, 28 modal) in roman numerals | **This is the Viterbi transition matrix the design left unspecified.** Roman numerals are key-relative, so the key estimate maps them to pitch classes directly. |
| `src/ChordSuggest.h` | Neo-Riemannian transforms, circle-of-fifths motion, diatonic degrees, chromatic substitutions | The alternates list. `DESIGN.md:187` says one click cycles a chord chip to the next-most-likely candidate; this enumerates what those candidates are. |

The Markov table is the best of these. A Viterbi over a flat transition prior is barely more
than per-frame argmax with hysteresis; over a real progression corpus it starts correcting
chords that the chroma got wrong, and the modal third of that corpus (Dorian, Mixolydian,
Lydian and Phrygian vamps) covers exactly the material the classical profiles fail on.

### 5.3 What has to change, honestly

None of it drops in unmodified.

- **`Chords.h` takes a set of sounding MIDI notes from a keyboard.** Quarry's input is a
  beat-synchronous chroma from a noisy transcription with wrong notes in it. The scoring
  function generalises to weights naturally (covered becomes a sum of weights, extras likewise),
  but it needs a weight threshold below which a pitch class is not considered present, and that
  threshold needs to come off the bench rather than off intuition. This is a port, not a copy.

- **The Markov corpus is hand-authored, and its own header says so.** `MarkovData.h:5-23`
  explains that Octavium's data never existed, so this corpus was written fresh. As a
  *generative* source that is fine. As a *prior over observations* it biases toward common pop
  progressions, which will hurt on jazz or unusual material. It also has no smoothing, stated
  at `ChordMarkov.h:213`, so an unseen transition is probability zero, which in a Viterbi means
  a legal progression can be rendered impossible. Add a floor before using it as a prior.

- **Beat-synchrony requires tempo**, so chords sequence after §3.2 regardless.

### 5.4 Licence, and where the shared code should live

Keys is **MIT**, Quarry is **Apache-2.0**. MIT into Apache-2.0 is clean with attribution
retained, and both are the same author's code.

That is worth stating plainly because `ANALYSIS.md` §3.1 is otherwise a wall: Essentia,
madmom, Chordino, libKeyFinder, aubio and QM-DSP are all copyleft or non-commercial, and the
conclusion there was to write the arithmetic rather than have the licence conversation. For
key and chords the arithmetic is already written and already owned. This is the one place in
the analysis stack where there is no licence conversation at all.

Both repos already vendor headers from `okstudio-juce-kit`, and `ScaleModes.h` notes it
deliberately sits alongside the kit's `okstudio/Scales.h` rather than merging with it. So the
open decision is whether the shared theory layer moves into the kit or gets vendored into
Quarry the way `ThirdParty/okstudio` already is. The kit is the right home if Quarry and Keys
are both going to evolve it; vendoring is right if Quarry only consumes. Worth deciding before
the first copy, not after the second.

There is a product-level symmetry here too, and it is one line: Quarry reads chords out of
audio, Keys plays them back. A chord Quarry detects is a chord Keys can receive as a pad.

---

## 6. Frequency and acoustic analysis, and why EDM needs it most

For a piano take the notes are most of the answer. For an electronic track they are a small
part of it, because what was actually made was a sound, an arrangement and a mix. All of the
following comes off the acoustic chain of §1 and none of it is available from the model path.

**Sound design, which is the part nothing else measures.**

- **Detune width.** A supersaw shows each partial smeared across a cluster of CQT bins rather
  than landing on one. The width in cents *is* the detune amount, readable straight off the
  peak spread. Nobody reports this and it is one of the two or three numbers that actually
  reproduce the patch.
- **Filter motion.** Track the spectral roll-off point (the frequency below which 85 % of the
  energy sits) per frame and you have the cutoff automation as a curve, drawable directly into
  a synth. A local peak sitting at that roll-off point is the resonance.
- **Envelope.** Time from onset to peak level is the attack in milliseconds; the decay slope
  after it is the decay and sustain. That maps onto ADSR controls without interpretation.
- **The kick.** Fundamental frequency, decay time, and the pitch envelope over the first tens
  of milliseconds. "A 48 Hz sine with a 380 ms tail, pitched down from 120 Hz over 40 ms" is a
  reproducible spec, and it is a short FFT at each kick onset.

**Mix and master.**

- **Sidechain.** Average the loudness curve over one beat period across the whole take and the
  ducking envelope emerges as a shape, with its depth in dB. That is the compressor curve, as a
  drawable, not an adjective.
- **Stereo by band.** Electronic mixing is largely "sub in mono, mids narrow, top wide".
  Mid/side energy per band measures exactly that, and it also explains the downmix cancellation
  of §1 when it happens.
- **Loudness and dynamics.** Integrated LUFS, true peak, crest factor per band. How hard the
  limiter is working, and where.

**Arrangement.**

- **Exact tempo and phase.** Electronic tempo is a machine constant, so the estimate should
  snap to it and report the residual as a confidence rather than reporting 128.03.
- **Loop length and section boundaries.** The self-similarity matrix of `DESIGN.md:361` row 11
  is a weak tool on rubato piano and a strong one here, because the material is regular by
  construction. Sections land on 8, 16 and 32 bars and the matrix finds them cleanly.
- **Band energy over time** as the arrangement map: when the sub enters, when the top opens up.

The honest caveat on all of it: these are measurements of *a* signal, not recovery of *the*
signal chain. Two different patches can produce the same roll-off curve. The output is a
starting point that is far better than listening and guessing, not a recovered project file,
and the report should say so in those words rather than implying otherwise.

---

## 7. Sequencing

`ANALYSIS.md` §4.0 asks for the bench before anything else, and profiles make that more true
rather than less: once a profile changes decoding there are two axes to regress instead of
one, and without a number there is no way to tell whether the piano profile helped piano or
merely moved the damage elsewhere.

1. **Tuning offset** (§3.5). Cheapest, and it is a correction that everything else depends on.
2. **The free statistics** (§3.1 to §3.3). No new DSP, and §3.2 produces the discriminator
   that §2.3 needs.
3. **Tempo and beat phase**. Chords, the bass histogram and beat-synchronous anything all
   block on it.
4. **Key rework** (§4). The `ANALYSIS.md` §2.1 velocity fix has landed, so the re-measurement
   is now the blocker and needs a labelled set to measure against; then profiles, then the bass
   histogram, then the ribbon.
5. **Chords** (§5), porting from `../Keys` with the three changes in §5.3, and the shared-code
   location decided before the first copy.
6. **Material profiles** (§2), once there is enough measurement to auto-propose one.
7. **The acoustic chain** (§1, §3.4, §6). Independent of all of the above and can run in
   parallel with it, since it shares no code with the model path. Start with provenance and
   loudness, which are a few hours, before the sound-design work of §6.
