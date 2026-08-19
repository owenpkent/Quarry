# Quarry Sampler — Design

*Capture what this computer is playing, know exactly where it came from, keep it.*

Status: built and in use as of 2026-08-16, bar the items still marked open under *Build
order*. This is the page Quarry opens on, with Transcribe reached from a capture rather than
from a tab beside it.

**It was designed as standalone-only and is not actually gated to the standalone.** Nothing
checks the wrapper type; the only guard is `#if JUCE_WINDOWS`, so the page is in the VST3 too
on Windows. That was harmless while it was a second tab. It is less harmless now that it is
the page the editor opens on and resizes its own window to fit, which in a host is a
negotiation rather than a decision. Either gate it or accept it on purpose; it should not stay
this way by omission.

Quarry's existing capture exists to feed a transcriber: it holds a take, converts it to
MIDI, and the audio is a by-product. The Sampler inverts that. The audio is the point, the
source is the metadata, and transcription is something you may never run.

---

## Decisions

| Question | Decision |
|---|---|
| Where it lives | The page Quarry opens on. Transcribe is downstream, reached from a capture |
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
| Library | Folder browser over the sidecars, with search, no index file. Can be turned off |

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

   Three things learned building it. Search `TreeScope_Subtree`, not `Descendants`: the
   omnibox is chrome, a few levels down, while a descendant search can walk into the
   rendered page and take as long as the page is large. Chrome hides the scheme, so the
   omnibox reads `youtube.com/watch?v=...` and the `https://` has to be put back for the
   sidecar to hold something pasteable. And the omnibox holds search queries as readily as
   addresses, so anything with a space in it is discarded rather than recorded as a URL.

   The cost worth naming: Chrome switches on renderer accessibility when a UIA client
   appears. That is paid by the browser, and it is why this is a deliberate call once at the
   start of a take rather than anything polled.
3. **Media session.** WinRT `GlobalSystemMediaTransportControlsSessionManager` gives title,
   artist, album and thumbnail for Spotify, browsers and most media apps. Sessions are keyed
   by AUMID, which does not map cleanly to a PID, so match by AUMID and treat a
   near-miss as no answer rather than a wrong one. Must be called off the message thread.
4. **Window screenshot.** `PrintWindow` with `PW_RENDERFULLCONTENT` first, falling back to
   Windows.Graphics.Capture for hardware-composited windows that come back black. Written
   as a `.png` beside the audio, downscaled to about 480 px wide.

The source picker lists **every window on the desktop**, with the ones currently making sound
at the top: `IAudioSessionManager2::GetSessionEnumerator` for the audible half, with a live
meter per row from `IAudioMeterInformation::GetPeakValue`, merged with an `EnumWindows` walk
for everything else. A filter box narrows it, matching on the application and on the window
title.

**Rank on the meter, not on the session state.** `AudioSessionStateActive` sounds like the
right sort key and is not: run the listing on this machine and Premiere, Resolve and a
wallpaper engine all report as active at a peak of exactly zero, holding a stream open
against the moment they need it. Of fifteen sessions, one was making sound. Sorting by peak
is what puts the row that matters first.

**Updated 2026-08-18: audible is an ordering, not a filter.** It used to be both, and a row
had to be making a sound to be listed at all. That got the common case exactly backwards. You
do not find a video and then decide to record it; you decide to record it and then press play,
and a picker that cannot see a paused tab makes you start the audio, race to Quarry, find the
row, and arm it, having already lost the opening. Process loopback never needed the target to
be audible: its stream is a clock, and a silent process delivers zero-filled packets at the
requested rate, so arming something quiet and waiting is supported rather than a trick. The
list is now everything you could point at, ordered by what is playing.

That trades a short list for a long one, so three things keep it usable: the filter box, one
row per window rather than per process (a browser with four windows is four rows, and the
title is what tells them apart), and exclusions for the things nobody means by "that window" -
owned and untitled windows, `WS_EX_TOOLWINDOW` palettes, the shell's desktop and its wallpaper
hosts, DWM-cloaked suspended UWP shells, and Quarry itself.

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

The page the window opens on. Transcribe is what existed before, untouched, and is now
somewhere a capture takes you rather than a peer: loading one hands it to the transcriber and
follows it there, and `< SAMPLES` in the toolbar is the way back. Sample is:

- **Source picker**: apps currently making sound, each with a live meter and a volume
  warning. An "everything" row selects the endpoint fallback.

  **One row per window, not per application.** Two browser windows are one process, and
  nothing about a pid says which of them made the sound. Asking the process and guessing
  backwards to a window picked whichever happened to be a few pixels wider, so the capture
  got named after the wrong tab. The rows are windows now and the choice travels into
  `SampleRecorder::start`, which is the same shape OBS uses and for the same reason. Note this
  fixes the *label*, not the audio: two windows of one process share one stream, and no
  per-window capture exists to be had.

  The application's name is printed once against the first of its windows rather than down
  every row, where it was a column of identical cells.
- **Record / stop**, plus a stereo level and elapsed time. The meter runs on its own 60 Hz
  tick rather than the source enumeration's 250 ms, because a level that updates four times a
  second reads as a run of jumps; it rises at once and falls on a 320 ms time constant, in
  real elapsed time so a late frame does not make it lurch. Two lanes, because one bar cannot
  tell a centred signal from one that has quietly lost a channel.
- **The captures**, which can be turned off. Hidden, the window shrinks to the source picker,
  the picker becomes a dropdown, and Quarry is a small capture tool; the choice is remembered.
  Shown, it is a browser over every sidecar under the captures root, read on a background thread
  and published back to the message thread. One search box rather than a row of filters,
  matched against name, application, window title, URL and tags at once, because the
  alternative is asking someone to know which field their memory of a sample lives in.
  Multiple words narrow. Reveal, delete to the recycle bin, and TRANSCRIBE, which hands the
  file to the other page and is the entire reason for living inside Quarry.

  **It browses the folders rather than flattening them.** Captures are written into dated
  folders, so the browser walks them: a month, then a day, then the takes, with a `..` row
  back up. The folders are derived from the scan rather than from a second walk of the disk,
  so one appears exactly when there is a capture inside it. Searching flattens the tree,
  because not knowing which folder a thing is in is the whole reason anyone types in that box.

  Rows carry what identifies a capture and nothing else: when it was taken, what the window
  said, and how long it runs. The application was on every row saying the same thing, and
  loudness belongs to using a sample rather than finding one; both are still in the sidecar.

  **There is no database, and that is a decision.** The sidecars are the record; the index
  is built from them and can be thrown away and rebuilt. Delete a capture in Explorer and it
  is gone from the browser too, with nothing left pointing at a file that is not there,
  which is the failure a database would have had.

  **Auditioning came for free.** The Transcribe page already owns a player, so handing a
  capture to it plays the capture. A browser with its own audio device would have been a
  second device open in a standalone that already has one.

Mouse-only, to the line's standard. Text search is there but never the only route to
anything: app, date and tag filters are all click targets, so a full session can happen
without the keyboard.

**The reverse is not true, and an audit on 2026-08-16 said so.** A full session cannot yet
happen without the mouse, and the page is largely closed to a screen reader:

- Neither list implements `ListBoxModel::getNameForRow`, so every row announces as "Row 1",
  "Row 2". JUCE's default is exactly that string. All of the content above, including the
  window titles that are the entire point of the per-window rows, is invisible to assistive
  technology.
- `< SAMPLES` is `setWantsKeyboardFocus(false)` and has no shortcut, while the TRANSCRIBE
  button that sends you there is focusable. That is a one-way door for a keyboard user.
- `r`, `m` and `c` in `QuarryMainView::keyPressed` trigger transport buttons unguarded, and
  `triggerClick()` works on a hidden one. From the Sample page they reach into Transcribe.
- The selection block, the status line, the column headings and the meter are `drawText`
  calls rather than components, so none of the state they carry reaches a screen reader.

The dropdown in the narrow window is the exception and is the most accessible control on the
page, because JUCE's `ComboBox` carries its own name, value and expanded state.

---

## Two things a security pass changed

Both were found reviewing this work before it was proposed, and both are recorded here
because they are decisions about what the product may do, not bugs that happened to be fixed.

**A sidecar's filenames are not trusted.** The `screenshot` field names a file, and that name
arrives inside a file which the whole sidecar design expects to travel between machines. JUCE
resolves `../` inside `getSiblingFile`, and the resolved path was later handed to
`moveToTrash`, so a crafted sidecar in a downloaded sample pack could have deleted something
else entirely on a single click of DELETE. `SampleLibrary::siblingNamed` now refuses
separators, `..` and drive prefixes, and then checks the resolved parent is the sidecar's own
folder. It is public rather than tucked away in the .cpp so it can be tested, because it is a
boundary rather than a convenience, and `Tests/sampler_test.h` covers the shapes an attack
would use.

**Endpoint mode gathers no visual identity.** Recording one application means describing the
one thing the person picked off a list. Recording everything means the program picks the
target, out of windows nobody saw, on the strength of being loudest for one instant. Reading
that window's address bar and photographing it is not covered by "record everything this
computer plays", and a banking tab that beeped at the wrong moment would have become a URL and
an image on disk that then travelled with any sample pack built from the folder. Endpoint mode
records the inferred application's name, which is all a guess needs, and leaves `url` and
`screenshot` null exactly as every other ungathered field already is.

A third, smaller thing came out of the same pass. `addressBarOf` used to take the first Edit
control in the window's subtree. The omnibox is reliably first today, but asking UI Automation
anything is what makes Chrome switch on renderer accessibility, and from the next call onward
the page's own fields are in that tree. It now walks the matches, skips anything marked as a
password, and accepts only a value that parses as an address.

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
2. ~~Source picker over audio sessions, with meters and the volume warning.~~ **Done.**
   `okstudio/WasapiProcessLoopback.h` in the kit carries the capture stream and `sessions()`;
   `Views/SamplePageView` is the picker. `QuarryMainView` first gained a two-page toggle and
   then lost it again: Sample is the page the window opens on, and the toggle became one
   `< SAMPLES` button that only exists on the page it returns from. The picker lists windows
   rather than processes, via `windowsOfSource()`.
3. Record / stop, buffer to RAM, trim and loudness, write float32 WAV plus sidecar.
   **Engine done**: `Quarry/Source/Sampler/`. `SampleMath.h` is the pure arithmetic, tested
   in `Tests/sampler_test.h`; `SampleMetadata.h` is the sidecar schema; `SampleRecorder`
   ties capture, trim, loudness and writing together. Captured audio is held in fixed
   blocks rather than one growing buffer, so a long take costs an occasional small
   allocation instead of copying everything so far, which at a hundred megabytes would drop
   audio on the floor.
4. Source identification, one signal at a time, each independently skippable.
   **Three of four done** in `Sampler/SourceIdentity`: process and window title, browser
   URL, and the window screenshot, all measured working against a live browser. The media
   session is the one left, and it is blocked rather than merely pending: SMTC needs
   C++/WinRT, and this project is on C++17 with no `/await`. Raising that is a build change
   to argue for on its own, not something to slip in beside a metadata field.
5. ~~Endpoint fallback path.~~ **Done.** The loudest session at the start of a take is
   written down as the guess, and the sidecar says `isolation: endpoint` so nothing reads a
   guess as a fact.
6. ~~Library browser: load, filter, reveal, delete.~~ **Done**, in `Sampler/SampleLibrary`
   and the right-hand half of the Sample page. Scanned off the message thread, filtered on one
   box against every field at once, and deletes go to the recycle bin. It browses the dated
   folders rather than flattening them, and can be turned off, which shrinks the window to the
   source picker. Tagging and renaming are the parts not built.
7. ~~Hand-off to the Transcribe page.~~ **Done**, through `SourceAudioManager::onFileDrop`,
   which is also how a capture gets auditioned: the Transcribe page already owns a player,
   so the browser never needed an audio device of its own.
8. Classification, decided as above.
9. **Accessibility**, listed under *The page*. The row names and the `< SAMPLES` focus are
   small and worth doing first; making painted state reachable is a design question.
10. **Decide whether this page belongs in the plugin**, per the note at the top. It is in the
    VST3 today because nothing stops it, which is not the same as intending it.
