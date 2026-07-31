# Video-Enhanced MIDI Transcription Proposal

> **Project:** Quarry  
> **Status:** Superseded in part. Kept as planning history, not as a specification.  
> **Date:** January 2026, annotated 2026-07-31

> **Read `docs/DESIGN.md` → *Roads not taken* and → *Licence constraints* before acting on
> anything below.** That later pass looked up the published numbers and the dependency
> licences and closed several of these paths. Three corrections, marked again in place:
>
> - **Path B is closed.** Its "Direct physical ground truth" / "Accuracy Gain: Very High"
>   claims are contradicted by the measurements. The best visual-only system (ViT, IJCAI
>   2025) scores onset F1 **0.68** on real YouTube top-down piano video where the audio-only
>   baseline scores **0.877** on the same clips, and that 0.68 is measured against
>   pseudo-labels generated from the audio, so the comparison already flatters it. Video buys
>   pitch accuracy and never timing: 30 fps quantises onsets to ±16 ms against basic-pitch's
>   11.6 ms hop. Building it would make transcriptions worse.
> - **Path A2 (sheet-music OMR) is closed on licensing.** Audiveris is AGPL; oemer is Python
>   plus deep models.
> - **The dependency table below understates the licence cost.** FFmpeg and OpenCV are both
>   refused: bundling FFmpeg means removing any reverse-engineering prohibition from the EULA
>   and hosting exactly-corresponding source next to every download forever, and OpenCV's
>   Windows decode path *is* a prebuilt LGPL `opencv_videoio_ffmpeg*.dll` wearing an
>   Apache-2.0 badge. The chosen route is Media Foundation, which Microsoft has already
>   licensed the codecs for. See `docs/PLAN.md` → Milestone 10.
>
> **Path A1 survives and is the kept road**, as `docs/PLAN.md` → Milestone 11, rebuilt clean
> room in C++ with no OpenCV and no deep learning: a Synthesia video is a deterministic render
> of a MIDI file, so recovering it is de-rendering rather than perception. The Python
> prototype in `prototypes/synthesia_detector/` has two known bugs and is not a baseline: it
> divides the keyboard into equal columns so it cannot emit a single black key
> (`synthesia_detector.py:120`), and it registers onsets when a block enters the *top* of the
> frame rather than at the strike line (`:133`, `:216`), so every onset is early by the full
> fall time and every duration inflated by it.
>
> Nothing in this document has been built. Quarry transcribes audio only.

---

## Executive Summary

Quarry currently uses audio-only analysis (Spotify's basic-pitch model) for MIDI transcription. This proposal explores **multimodal approaches** that incorporate video data to improve transcription accuracy. Two primary video sources are considered:

1. **Sheet music tutorial videos** — Scrolling/highlighted notation synced to audio
2. **Piano performance videos** — Visual detection of keys being pressed

Both approaches can provide ground-truth or supplementary signals that disambiguate audio-only transcription errors.

---

## Current Architecture Overview

Quarry's transcription pipeline:

```
Audio Input → CQT Features → CNN (basic-pitch) → Posteriorgrams → Note Events → MIDI
```

Key components in `Lib/Model/`:
- **Features.cpp/h** — CQT + Harmonic Stacking
- **BasicPitchCNN.cpp/h** — Neural network inference via RTNeural
- **Notes.cpp/h** — Posteriorgram → note event conversion

The video enhancement would add a **parallel visual pipeline** that merges with the audio pipeline at the note event or posteriorgram stage.

---

## Proposal Path A: Sheet Music OCR + Alignment

### Concept

Extract note information from sheet music displayed in YouTube tutorial videos, then use audio-visual alignment to synchronize the extracted notes with the audio timeline.

### Data Sources

- **Synthesia-style videos** — Falling blocks representing notes (easiest)
- **Scrolling sheet music** — Traditional notation with playback cursor
- **Static sheet music with highlighting** — Notes illuminate as played

### Implementation Phases

#### Phase A1: Synthesia Block Detection
**Complexity:** Low-Medium  
**Accuracy Gain:** High for piano tutorials

1. Frame extraction from video
2. Color/position detection of falling blocks
3. Map vertical position → time, horizontal position → pitch
4. Direct MIDI generation (no audio needed for this path)

**Tech Stack:**
- OpenCV for frame processing
- Simple CNN for block segmentation
- Heuristic pitch mapping (column → MIDI note)

#### Phase A2: Sheet Music OCR
**Complexity:** High  
**Accuracy Gain:** Medium-High

1. Detect staff lines and notation symbols
2. Use OMR (Optical Music Recognition) models
3. Output MusicXML or MIDI
4. Align to audio using DTW (Dynamic Time Warping)

**Tech Stack:**
- [Audiveris](https://github.com/Audiveris/audiveris) — Open-source OMR
- [oemer](https://github.com/BreezeWhite/oemer) — End-to-end OMR with deep learning
- [music21](https://github.com/cuthbertLab/music21) — Music analysis library

#### Phase A3: Audio-Visual Fusion
**Complexity:** Medium  
**Accuracy Gain:** Depends on alignment quality

1. Run both audio transcription (existing) and visual transcription
2. Align timelines using:
   - Audio fingerprinting
   - DTW on onset patterns
   - Cross-correlation of note density
3. Merge results with confidence weighting

---

## Proposal Path B: Piano Key Detection

> **Closed.** See the correction at the top of this document. The accuracy figures in this
> section are asserted, not measured, and the measured ones point the other way.

### Concept

Detect which piano keys are being pressed in performance videos, extract MIDI-equivalent events, and fuse with audio transcription.

### Data Sources

- **Overhead camera views** — Clear key visibility
- **Side-angle views** — Requires perspective correction
- **Hand-tracking videos** — Infer notes from finger positions

### Implementation Phases

#### Phase B1: Key State Detection (Overhead View)
**Complexity:** Medium  
**Accuracy Gain:** Very High (clean videos)

1. Detect piano keyboard in frame (homography/template matching)
2. Segment individual keys
3. Detect pressed state via:
   - Color change (key depression shadow)
   - Motion detection (key movement)
   - CNN classifier per key region
4. Map key positions to MIDI notes

**Tech Stack:**
- OpenCV for preprocessing
- YOLO or similar for keyboard detection
- Custom CNN for key state classification
- MediaPipe for optional hand tracking

#### Phase B2: Hand/Finger Tracking
**Complexity:** High  
**Accuracy Gain:** Medium (occlusion issues)

1. Use MediaPipe Hands for finger landmark detection
2. Map fingertip positions to key regions
3. Infer pressed keys from finger contact

**Challenges:**
- Finger occlusion
- Varying hand sizes
- Requires accurate keyboard calibration

#### Phase B3: Multi-View Fusion
**Complexity:** Very High  
**Accuracy Gain:** Highest potential

1. Support multiple camera angles
2. 3D reconstruction of hand/key interactions
3. Resolve occlusions via view synthesis

---

## Proposal Path C: Hybrid Audio-Visual Model

### Concept

Train a single multimodal neural network that takes both audio spectrograms and video frames as input, learning to fuse information end-to-end.

### Implementation Phases

#### Phase C1: Dataset Creation
**Complexity:** High (data collection)  
**Prerequisite for:** C2, C3

1. Collect paired audio-video-MIDI data:
   - MIDI-controlled piano with video recording
   - Synthesia videos with known MIDI
   - YouTube tutorials with manually verified transcriptions
2. Create alignment annotations
3. Build data pipeline for training

**Estimated Dataset Size:** 100+ hours for meaningful training

#### Phase C2: Two-Tower Architecture
**Complexity:** High  
**Accuracy Gain:** Potentially very high

```
Video Frames → Video Encoder (CNN/ViT) ─┐
                                        ├→ Fusion Layer → Note Predictions
Audio (CQT)  → Audio Encoder (existing) ┘
```

1. Keep existing basic-pitch audio encoder
2. Add video encoder (pretrained backbone)
3. Train fusion layers on paired data
4. Attention mechanism for temporal alignment

**Tech Stack:**
- PyTorch for model development
- Export to ONNX for inference
- Video encoder: TimeSformer, Video Swin, or SlowFast

#### Phase C3: End-to-End Multimodal Transformer
**Complexity:** Very High  
**Accuracy Gain:** Highest ceiling

1. Unified transformer architecture
2. Audio and video tokens in same sequence
3. Cross-modal attention
4. Pretrain on large music video corpus

**Research-level effort** — Likely requires significant compute and expertise.

---

## Proposal Path D: Post-Processing Enhancement

### Concept

Use video analysis as a **correction layer** on top of existing audio transcription, rather than deep model integration.

### Implementation Phases

#### Phase D1: Confidence-Based Correction
**Complexity:** Low-Medium  
**Accuracy Gain:** Moderate

1. Run audio transcription (existing)
2. Run video analysis (Path A or B)
3. For notes with low audio confidence:
   - Check if video confirms the note → boost confidence
   - Check if video contradicts → suppress note
4. Add notes detected in video but missed in audio

#### Phase D2: Error Pattern Learning
**Complexity:** Medium  
**Accuracy Gain:** Moderate-High

1. Collect examples where audio transcription fails
2. Identify visual cues that predict these errors
3. Train lightweight classifier to flag uncertain regions
4. Apply targeted video analysis only where needed

**Advantage:** Efficient — only processes video when beneficial.

---

## Comparison Matrix

| Path | Complexity | Accuracy Potential | Data Requirements | Inference Speed |
|------|------------|-------------------|-------------------|-----------------|
| A1: Synthesia | Low | High (specific videos) | Low | Fast |
| A2: Sheet OCR | High | Medium-High *(closed: AGPL)* | Medium | Medium |
| B1: Key Detection | Medium | ~~Very High~~ **measured worse than audio-only** | Medium | Medium |
| B2: Hand Tracking | High | Medium | Low | Slow |
| C2: Two-Tower | High | Very High | High | Slow |
| D1: Post-Process | Low-Medium | Moderate | Low | Fast |

---

## Recommended Starting Path

### For Quick Wins: **Path A1 (Synthesia Detection)**

- Immediately useful for piano tutorial videos
- Relatively simple computer vision
- Can run standalone or enhance audio transcription
- Large corpus of existing Synthesia videos on YouTube

### ~~For Highest Accuracy: **Path B1 (Key Detection)**~~ (withdrawn)

This section claimed "direct physical ground truth". It is not: the published visual-only
onset F1 is 0.68 against audio-only's 0.877 on the same clips, and 30 fps cannot resolve
onsets better than ±16 ms. Withdrawn, and left visible so it is not proposed again.

### For Research/Long-term: **Path C2 (Two-Tower)**

- Highest accuracy ceiling
- Requires significant dataset and training
- Could become a differentiating feature

---

## Technical Prerequisites

### New Dependencies

| Component | Library Options | License | Status |
|-----------|-----------------|---------|--------|
| Video decoding | FFmpeg, OpenCV | LGPL/BSD | ❌ refused, see the note at the top; Media Foundation instead |
| Frame processing | OpenCV | BSD | ❌ refused: its Windows decode path is a prebuilt LGPL FFmpeg DLL |
| Object detection | YOLO, Detectron2 | Various | not needed by the kept path |
| Hand tracking | MediaPipe | Apache 2.0 | Path B2, closed |
| OMR | Audiveris, oemer | AGPL/MIT | Path A2, closed on the AGPL |
| ML inference | ONNX Runtime (existing) | MIT | in the tree, and it physically cannot load a second model |

### Integration Points

1. **New module:** `Lib/Video/` for video processing
2. **Extended UI:** Video file input, frame preview
3. **New model weights:** Video encoder, fusion layers
4. **Build system:** Add OpenCV/FFmpeg dependencies

---

## Open Questions

1. **Plugin vs. Standalone:** Should video features be in the plugin or a separate preprocessing tool?
2. **Real-time vs. Offline:** Video processing is likely offline-only due to latency
3. **YouTube Integration:** Direct URL input vs. requiring local files?
4. **Licensing:** Some OMR tools are AGPL — compatibility concerns?

---

## Next Steps

- [ ] **Prototype A1** — Synthesia block detection proof-of-concept
- [ ] **Prototype B1** — Piano key detection on sample videos
- [ ] **Dataset survey** — Identify existing paired audio-video-MIDI datasets
- [ ] **Benchmark** — Measure current audio-only accuracy on test set
- [ ] **User research** — Which video types are most common for target users?

---

## References

- [basic-pitch paper](https://arxiv.org/abs/2203.09893) — Current audio model
- [Piano Transcription with Pedals](https://arxiv.org/abs/2010.01815) — State-of-art audio transcription
- [Audiveris OMR](https://github.com/Audiveris/audiveris) — Open-source sheet music recognition
- [MediaPipe Hands](https://google.github.io/mediapipe/solutions/hands.html) — Hand tracking
- [MAESTRO Dataset](https://magenta.tensorflow.org/datasets/maestro) — Piano audio + MIDI pairs

---

*Document created for Quarry project planning.*
