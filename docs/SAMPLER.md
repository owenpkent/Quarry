# Quarry Sampler — Design

*Capture what this computer is playing, know exactly where it came from, keep it.*

Status: design settled 2026-08-16. Nothing built yet. This is a second page inside the
Quarry **standalone** app, not a separate product and not available in the plugin.

Quarry's existing capture exists to feed a transcriber: it holds a take, converts it to
MIDI, and the audio is a by-product. The Sampler inverts that. The audio is the point, the
source is the metadata, and transcription is something you may never run.

---

## Decisions

| Question | Decision |
|---|---|
| Where it lives | Second page in the Quarry standalone, toggled from the toolbar |
| Capture isolation | Per-process WASAPI loopback by default, whole-endpoint as fallback |
| Trigger | Explicit record / stop. No rolling buffer, nothing runs until you press record |
| Concurrency | One source at a time |
| Format | 32-bit float WAV, bit-exact, no conversion at any point |
| Metadata | BWF + RIFF INFO chunks in the WAV, full record in a `.json` sidecar. No database |
| Source ID | Process + window title, browser tab URL, media session (track/artist), window screenshot |
| Layout | Date first: `Samples/2026-08/2026-08-16/` |
| Naming | `143052-chrome-never-gonna-give-you-up.flac` |
| Post-capture | Trim edge silence, measure loudness. No fades, no loop detection |
| Quarry analysis | Nothing automatic. Key, tempo and MIDI are one click, never a default |
| Semantic tagging | Deferred, with a schema slot reserved. See *Open: classification* |
| Library | Full browser with search, over the sidecars, no index file |

---

## Capture

### Per-process loopback

`ActivateAudioInterfaceAsync` against `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`, with
`AUDIOCLIENT_ACTIVATION_PARAMS.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
and `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`. Windows 10 2004 and later, so it
is a hard Windows-only feature and the page hides itself elsewhere.

**Targeting is always tree-based, and the alternative is not what it sounds like.**
`PROCESS_LOOPBACK_MODE` has exactly two values: `INCLUDE_TARGET_PROCESS_TREE` and
`EXCLUDE_TARGET_PROCESS_TREE`. There is no mode that captures one process without its
children, so the earlier worry about browsers rendering from a child process is moot: the
tree is the only unit of inclusion there is.

`EXCLUDE` is not the opposite of `INCLUDE` on the same target. It captures everything the
machine plays *except* that tree, which is a separate feature worth remembering later
(record the machine but not Discord, or not Quarry itself). It is not a fallback.

In practice the pid to target is the one the session enumerator reports, which is the
process that owns the render session. Measured: that pid captures Chrome correctly with no
parent-process resolution needed.

This path sits outside JUCE's device layer entirely. It is raw WASAPI on its own thread,
writing into a lock-free FIFO that the page drains. It never touches the standalone's own
audio device, exactly as the existing SOURCE strip is careful not to.

The format is caller-specified rather than negotiated, and the OS converts to whatever we
ask for rather than refusing: measured, asking for 48 kHz on a 44.1 kHz endpoint opens and
delivers 48 kHz. So ask for the **default endpoint's mix format** and no conversion happens
at all. Do not assume what that format is. This machine's endpoint mixes at 44.1 kHz, not
the 48 kHz the first draft of this document guessed.

### Fallback

Whole-endpoint loopback, which Quarry already has through the System Audio driver. Used
when the target predates process loopback, when a source cannot be resolved to a PID, or
when you deliberately want everything the machine is mixing. Attribution in that mode is
inferred and the sidecar says so (`isolation: "endpoint"`).

### The volume trap

Both loopback modes capture **after** an app's own session volume. A YouTube tab at 50%
bakes in 6 dB of loss that no format choice can undo. So the source picker reads each
session's volume via `ISimpleAudioVolume` and shows a warning next to anything below 100%,
with a one-click fix. This is the single most likely cause of a disappointing capture.

**Measured, and it is post-volume.** Halving Chrome's session volume halfway through one
continuous 12 second capture dropped the second half by 4.2 dB rms. Not the 6 dB a linear
halving predicts, because the material differs between halves and the slider ramps, but far
outside any natural variation and unambiguous in direction. The warning is required.

### Why focus does not matter

You do not need to watch the source while recording. Arm Chrome, press record in Quarry,
switch to Chrome, play, switch back, stop. Because the capture follows Chrome's process
tree rather than the endpoint, nothing you do in between (a notification, a Discord ping,
Quarry's own UI) lands in the file. The only cost is a few seconds of silence on the tail,
which the trim pass removes.

### What the spike measured

`tools/loopback_spike/` is a standalone console tool, no JUCE, that answers the questions
this design opened. Run `--list` for everything with an audio session, `--pid <n>` to
capture it. Results on this machine, 2026-08-16:

| Check | Result |
|---|---|
| Isolated capture of one app | Works. 6 s of Chrome, 5.99 s written, peak -5.68 dBFS, nothing else present |
| Wall-clock accuracy | 11.98 s written for 12.00 s, across three runs. No drift |
| Silence handling | A silent target still clocks: 220059 zero frames over 5 s, none flagged silent, no gaps |
| Non-mix rate | 48 kHz requested on a 44.1 kHz endpoint opens and delivers 48 kHz. Windows converts |
| Session volume | Post-volume. Halving mid-capture cost 4.2 dB rms |
| Endpoint mix format | 44.1 kHz here, so read it rather than assuming |

The silence result is the one that differs from endpoint loopback, whose header notes that
"nothing arrives while the endpoint is idle". Process loopback does deliver packets while
its target is silent, so the position-gap padding that path needs never fired here. Keep a
gap guard as insurance, expect it never to trigger.

---

## Source identification

Four signals, each degrading independently. Any one failing leaves the others intact, and
every field is nullable in the schema.

1. **Process and window.** `QueryFullProcessImageNameW` for the exe path, version-info for
   the friendly product name, `EnumWindows` filtered by PID for the largest visible
   top-level window, `GetWindowTextW` for its title. Cheap, always works.
2. **Browser tab URL and title.** UI Automation: `ElementFromHandle` on the browser window,
   then the omnibox Edit control's `ValuePattern`. The omnibox is native UI, so this does
   not require renderer accessibility. It is the most fragile signal here and breaks
   silently across browser updates, so it falls back to the window title without comment.
3. **Media session.** WinRT `GlobalSystemMediaTransportControlsSessionManager` gives title,
   artist, album and thumbnail for Spotify, browsers and most media apps. Sessions are keyed
   by AUMID, which does not map cleanly to a PID, so match by AUMID and treat a
   near-miss as no answer rather than a wrong one. Must be called off the message thread.
4. **Window screenshot.** `PrintWindow` with `PW_RENDERFULLCONTENT` first, falling back to
   Windows.Graphics.Capture for hardware-composited windows that come back black. Written
   as a `.png` beside the audio, downscaled to about 480 px wide.

The source picker itself lists **apps currently making sound**, not all processes:
`IAudioSessionManager2::GetSessionEnumerator`, with a live meter per row from
`IAudioMeterInformation::GetPeakValue`. You pick from a short list of things you can see
moving.

**Rank on the meter, not on the session state.** `AudioSessionStateActive` sounds like the
right filter and is not: run the listing on this machine and Premiere, Resolve and a
wallpaper engine all report as active at a peak of exactly zero, holding a stream open
against the moment they need it. Of fifteen sessions, one was making sound. Sorting by peak
is what makes the list short.

---

## Files

Three files per sample, sharing one stem:

```
Samples/2026-08/2026-08-16/143052-chrome-never-gonna-give-you-up.wav
                          /143052-chrome-never-gonna-give-you-up.json
                          /143052-chrome-never-gonna-give-you-up.png
```

Stem is `HHMMSS-<source-slug>-<title-slug>`, slugified to `[a-z0-9-]`, title capped at 48
characters, collisions resolved with `-02`. Time first so the folder sorts chronologically.

### Why float WAV and not FLAC

Loopback hands us 32-bit float, and JUCE's WAV writer takes it natively: `bitsPerSample`
of 32 sets `usesFloatingPointData` and writes format tag 3, `WAVE_FORMAT_IEEE_FLOAT`. So
the file is byte-for-byte what Windows gave us, with no conversion step anywhere.

That deletes a mechanism rather than swapping a format. FLAC is an integer codec, so it
would have needed a true-peak scan, a decision about captures legally exceeding 0 dBFS, and
a reversible gain factor stored in the tags. None of that exists now. Nothing ever touches
the audio.

Three more reasons, in order:

1. **Ableton decodes FLAC to a temp file.** Live does read FLAC, and has for many
   versions, so this is not a compatibility problem. But the manual is explicit that to
   play a compressed sample "Live decodes the sample and writes the result to a temporary,
   uncompressed sample file". Every drag costs a decode and a temp copy, and the
   uncompressed audio ends up on disk regardless. That erases most of what FLAC was saving.
2. **Universal everywhere else.** Float WAV is read directly by every DAW, hardware sampler
   and command-line tool. FLAC support is good but not total, particularly on hardware.
3. **Consistent with Quarry**, whose SAVE TO bar already writes WAV.

The costs, stated plainly: about 3x the disk (23 MB/min stereo at 48 kHz, so a 30 second
sample is 11.5 MB against roughly 4 MB for FLAC), and embedded tags become a fixed
vocabulary rather than FLAC's arbitrary key/value pairs. The second costs nothing here
because the sidecar is the complete record and the thing the browser reads.

One guard this adds: WAV tops out at 4 GB, about 2.9 hours of float32 stereo. Explicit
record/stop will not reach that by accident, but a forgotten capture must be stopped rather
than allowed to produce a corrupt file.

**Why not 32-bit FLAC**, since it was asked: the bundled libFLAC is 1.4.3 and does support
32-bit integer encoding, but JUCE caps it. `juce_FlacAudioFormat.cpp` returns `{16, 24}`
from `getPossibleBitDepths()`, rejects anything else in `createWriterFor`, and clamps with
`jmin(24, bitsPerSample)` at the encoder call. Reaching 32 means patching a JUCE module and
carrying that patch forever, and it would not buy bit-exactness anyway, because 32-bit FLAC
is integer and float32's moving exponent cannot be represented in fixed point at any word
length.

### Post-capture

Runs on the captured buffer before the FLAC is written, both in-process and fast:

- **Trim edge silence** to a threshold, storing `trim.startSec`, `trim.endSec` and
  `trim.originalDurationSec` so the crop is legible after the fact.
- **Measure loudness**: sample peak, 4x-oversampled true peak, and integrated LUFS
  (BS.1770-4, K-weighted and gated). Written to tags, applied to nothing.

Explicitly not doing: edge fades, loop-point detection, normalization.

---

## Metadata schema

Written into the WAV via JUCE's `WavAudioFormat` metadata map, and in full to the JSON
sidecar. The WAV carries a flattened human-readable subset so the file self-describes
anywhere: BWF `bext` for description, originator and origination date/time, and RIFF INFO
for title/artist/comment/software. The sidecar is the complete record and the thing the
browser reads.

**No DAW will read any of it.** Ableton in particular ignores ACID chunks outright (its own
docs say ACID loop tags "can not be used in Live"), reads no embedded tempo or key from any
format, and instead runs its own analysis into a `.asd` sidecar it writes next to the
sample. So embedded tags are for other tools and for the file being self-describing when it
travels, never a channel to Live. Expect `.asd` files to appear alongside our `.json` and
`.png` in every sample folder.

JUCE's WAV writer does expose the ACID chunk (`acidRootNote`, `acidTempo`, `acidBeats`,
`acidOneShot`), which FL Studio, Cubase, Studio One and Reaper do read. Worth filling in if
analysis ever becomes automatic, worth nothing for Ableton.

```jsonc
{
  "schema": 1,
  "capturedAt": "2026-08-16T14:30:52+01:00",
  "source": {
    "isolation": "process",          // "process" | "endpoint"
    "processName": "chrome.exe",
    "processPath": "C:/Program Files/Google/Chrome/Application/chrome.exe",
    "productName": "Google Chrome",
    "pid": 18244,
    "windowTitle": "Rick Astley - Never Gonna Give You Up - YouTube",
    "url": "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
    "sessionVolume": 1.0,            // warn on capture if < 1.0
    "screenshot": "143052-chrome-never-gonna-give-you-up.png"
  },
  "media": {                          // null when no SMTC session matched
    "title": "Never Gonna Give You Up",
    "artist": "Rick Astley",
    "album": "Whenever You Need Somebody"
  },
  "audio": {
    "sampleRate": 48000,
    "channels": 2,
    "format": "wav-float32",
    "bitDepth": 32,
    "durationSec": 12.44,
    "peakDb": -1.2,
    "truePeakDb": -0.9,
    "lufs": -14.3
  },
  "trim": { "startSec": 0.31, "endSec": 12.75, "originalDurationSec": 13.02 },
  "analysis": null,                   // key / tempo / MIDI, filled only on request
  "classification": null,             // reserved, see below
  "tags": []                          // user tags from the browser
}
```

`schema` is versioned from day one because the two null blocks are going to be filled.

---

## The page

A second mode in the standalone's window, switched from the toolbar. Transcribe is what
exists today and is untouched. Sample is:

- **Source picker**: apps currently making sound, each with a live meter and a volume
  warning. An "everything" row selects the endpoint fallback.
- **Record / stop**, plus level and elapsed time. Same shape as the existing toolbar so
  there is nothing new to learn.
- **Library browser**: every sidecar under the samples root, loaded on a background thread
  at startup and populated progressively. Filter by app, by date, by tag. Play, rename,
  reveal in Explorer, delete. A "Transcribe this" button hands the sample to the other
  page, which is the entire reason for living inside Quarry.

Mouse-only, to the line's standard. Text search is there but never the only route to
anything: app, date and tag filters are all click targets, so a full session can happen
without the keyboard.

---

## Open: classification

Auto semantic classification is wanted and deliberately unresolved. It costs nothing to
defer because nothing in the capture path depends on it: the sidecar reserves a
`classification` block, and any model reads finished WAVs later.

The decision gets made against real data, not benchmarks. Once there are 50 to 100 real
captures, run CLAP and a YAMNet-class tagger over that exact set offline on the 5090 and
compare the two tag lists side by side. Criteria, in order:

1. Do the tags describe *these* samples usefully, or is the vocabulary the wrong shape?
2. Licence. Quarry is Apache-2.0 and `DESIGN.md` already cut a feature over licensing.
3. Whether open-vocabulary text search ("dark analog bass") and sample-to-sample similarity
   are worth a model roughly 40x larger than a fixed-class tagger.

If CLAP wins, its embedding goes in the sidecar (about 512 floats) and the browser loads
them into RAM at startup, which is how search works without ever adding a database.

---

## Build order

1. ~~**Spike**: process loopback into a WAV, one hardcoded PID.~~ **Done 2026-08-16**, see
   `tools/loopback_spike/`. Findings are folded into the sections above; the one that
   changes code is that process loopback clocks continuously, so the endpoint path's
   silence-padding machinery is not needed here.
2. Source picker over audio sessions, with meters and the volume warning. **Backend done**:
   `okstudio/WasapiProcessLoopback.h` in the kit carries both the capture stream and
   `sessions()`, vendored into `ThirdParty/okstudio`. The JUCE panel is what remains.
3. Record / stop, buffer to RAM, trim and loudness, write float32 WAV plus sidecar.
   **Engine done**: `Quarry/Source/Sampler/`. `SampleMath.h` is the pure arithmetic, tested
   in `Tests/sampler_test.h`; `SampleMetadata.h` is the sidecar schema; `SampleRecorder`
   ties capture, trim, loudness and writing together. Captured audio is held in fixed
   blocks rather than one growing buffer, so a long take costs an occasional small
   allocation instead of copying everything so far, which at a hundred megabytes would drop
   audio on the floor.
4. Source identification, one signal at a time, each independently skippable:
   process/window, then screenshot, then SMTC, then browser URL last since it is the
   most fragile.
5. Endpoint fallback path.
6. Library browser: load, filter, play, reveal, delete, tag.
7. Hand-off to the Transcribe page.
8. Classification, decided as above.
