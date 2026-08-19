# Changelog

All notable changes to Quarry are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- feat: any window is recordable, playing or not. The source picker used to list only
  applications making a sound, which had the common case backwards: you do not find a video
  and then decide to record it, you decide to record it and then press play. It now lists
  every window on the desktop, with the audible ones and their meters sorted to the top, a
  filter box to get through them, and one row per window rather than per process. Process
  loopback never needed an audible target - its stream is a clock, and a silent process
  delivers zero-filled packets at the requested rate - so arming something quiet and then
  starting it is supported rather than a trick. `docs/SAMPLER.md` carries the reasoning and
  the list of what is excluded.
- feat: the Transcribe page says what it heard instead of drawing it. The piano roll gave up
  the lower half of the page to eighty-eight lanes of mostly nothing, and answered no question
  worth asking: the notes are not editable here and they are on their way to a host, so it was
  a picture of the export rather than a judgement of it. In its place is a readout - key with
  its runner-up, tempo, meter, note count, length - over a per-bar confidence strip built from
  the model confidence that was computed and thrown away on every take since the fork. Click a
  bar to seek to it, or press NEXT SHAKY BAR to walk the ones worth checking. The roll is one
  click away and the waveform, which is the transport, takes the height it used to spend.
- fix: the confidence strip ranks rather than judges. Absolute cutoffs were tried first and do
  not survive contact with the decoder's per-take derived thresholds: the first real capture
  run through them came back with all thirty-one bars red. The tiers are now fractions of the
  take's own median bar, so an even take reads as even and a real slump still stands out, and
  each bar's raw number is on its tooltip.
- refactor: the detected key is reported in one place. It was in the Scale Quantize panel and
  is now in the summary with the rest of what describes a take; the button that adopts it went
  with it. Two readouts of one number is one of them being out of date.

### Added

- feat: an out-of-process transcription sidecar, and the app can use it. Set
  `QUARRY_SIDECAR_CMD` and every take transcribes through a separate Python process running a
  real model (kong, transkun or muscriptor, chosen by `QUARRY_SIDECAR_ENGINE`), measured
  through Quarry's own pipeline at onset F1 0.98, velocity F1 0.96+, and pedal F1 0.83 to
  0.90 on real piano recordings, against the built-in engine's 0.775 / 0.275 / nothing. Basic
  Pitch stays in-process as the default and the automatic per-take fallback, so an unset
  variable means current behaviour byte for byte, and a sidecar failure costs one take, not
  the session. Out of process is also the licence boundary: non-commercial weights never
  enter Quarry's binary. `docs/SIDECAR.md` is the manual; `tools/sidecar/PROTOCOL.md` is the
  wire format; `Lib/Sidecar/SidecarClient` is the client, on native pipes because
  `juce::ChildProcess` cannot write a child's stdin.
- feat: sustain pedal exists. Sidecar takes carry CC64 into exported MIDI through
  `MidiFileWriter`, and the bench grew a `pedal` column: span-level F1 at the 200 ms
  tolerance the literature uses, against ground truth that now includes each window's CC64
  stream, mid-window pedal state included. No Quarry export has ever contained a pedal event
  before this.
- feat: the bench measures real material, and answers to mir_eval. Five new corpora beside
  the synthetic one: rendered MAESTRO performances, the same 22 windows as real recordings,
  SMD's Disklavier recordings, BabySlakh mixes, and a stratified render of the local GM MIDI
  collection, all deterministic, all gitignored, all rebuilt by pinned-and-hashed fetch
  scripts (`docs/BENCH.md`). `--dump-notes` exports what the engine heard;
  `tools/bakeoff/crosscheck_mir_eval.py` matched the bench's onset arithmetic against
  `mir_eval` exactly, so the numbers are literature-comparable; `--sidecar`/`--engine` run
  any external model through identical scoring. What it found is recorded in `ANALYSIS.md`
  §4.0 and §4.2, starting with: the synthetic corpus flattered the engine by 15 points, and
  pedal density predicts almost all of the damage.
- feat: a key bench, waiting on its measurement. `tools/keybench/` fetches the GiantSteps Key
  dataset (604 labelled EDM excerpts, labels committed so the harness survives upstream),
  rebuilds `KeyEstimate`'s exact histogram from bench dumps, and scores four key-profile sets
  by accuracy and the MIREX weighted score. Its self-test proves the relative major/minor
  blind spot `STATS.md` predicted. No key rework ships ahead of this number.

- feat: exported velocity is measured from the audio instead of taken from the model. The number
  written into every `.mid` Quarry has produced was `Notes::Event::amplitude`, the mean note
  posteriorgram over the note, which is the model's confidence and says nothing about how hard a
  key was struck. On piano that is close to fatal, because voicing, accents and the melody sitting
  above the accompaniment are carried by dynamics and nothing else. `Lib/Model/NoteVelocity` reads
  the harmonic-stacked CQT the feature extractor already produced, sums each pitch's fundamental
  and first two harmonics, peaks over the attack, and maps the take's own spread onto velocity.
  Not the span's RMS, which contains every other note sounding at the same time and reads a quiet
  note held under a loud chord as loud. Note-level velocity F1 on the bench goes 0.372 to 0.780.
- feat: a transcription bench, in `tools/bench`. Note-level precision, recall and F1 against
  reference MIDI at the standard 50 ms onset tolerance, three ways, plus mean onset error, over a
  corpus of paired files, in one command against a committed baseline. `--legacy` runs the engine
  as it stood before these changes, so a difference is attributable to the fixes rather than to
  the corpus, and `--baseline` exits non-zero when aggregate F1 falls. `make_corpus.py` writes a
  synthetic piano corpus with exact ground truth, so it is runnable before anyone has downloaded
  MAESTRO. Configure with `-DQUARRY_BUILD_BENCH=ON`. It found that the first cut of these fixes
  was a net regression, which is the entire argument for having it.

- feat: pick the window, not just the application. Two browser windows are one process, and
  nothing about a process id says which of them made the sound, so the old guess took whichever
  window happened to be a few pixels wider and named the capture after the wrong tab. Each
  window is its own row now, told apart by what it is showing. This gets the name right; the two
  still share one audio stream, and no per-window capture exists to be had.
- feat: the captures can be turned off. The window shrinks to the source picker, which becomes a
  dropdown, and Quarry folds down to a small capture tool. It reopens the way you left it.
- feat: a stereo record meter that moves. It runs on its own 60 Hz tick instead of the 250 ms
  the source list is enumerated on, and falls on a time constant rather than a step per frame,
  so a busy moment no longer makes it stall and lurch. Two lanes, because one bar cannot tell a
  centred signal from one that has quietly lost a channel.

- feat: sample one application, not the whole computer. The standalone gains a second page,
  **SAMPLE**, switched from the toolbar. It lists what is making a sound right now with a
  meter each, and records the one you pick in isolation: a browser tab and nothing else, no
  notification, no Discord ping. Because it follows that application rather than the speakers,
  you can start the take, switch to the source, play it, and switch back without any of that
  landing in the file. Windows 10 build 20348 and later; **Everything this computer plays** is
  there as a fallback, and says outright that its source is a guess.
- feat: a captured sample says where it came from. The application, the window title, the
  browser tab's URL and a picture of the window, written into the wav where the format allows
  and in full to a `.json` beside it. The sidecar is the record: there is no database, so
  deleting a sample in Explorer leaves nothing behind pointing at it.
- feat: the captures you have made, in one list. Search matches the name, the application, the
  window title, the URL and the tags at once, because knowing which field a memory lives in is
  not something anyone should have to do. **TRANSCRIBE** hands a capture to the other page,
  which is also how you listen to one.
- feat: an application at half volume is called out before you record it, in orange, with a
  button that sets it back. Loopback captures after an app's own volume slider, so 50% is 6 dB
  of loss baked into the file that no format can undo. It is the one thing about a capture that
  cannot be fixed afterwards.
- feat: captures are 32-bit float wav, exactly as the audio arrived, with no conversion, no
  dither and no decision about peaks above 0 dBFS. Silence is trimmed from both ends and the
  crop recorded, and peak, true peak and integrated loudness are measured and written down
  without anything being applied to the audio.
- feat: keep a take without opening a dialog. A **SAVE TO** bar along the bottom writes the
  recorded audio and the transcription to a folder picked once, with a toggle for each format.
  The name of the next take is shown before you commit to it, and nothing is ever overwritten.
  Both files of a take always share one name. A dropped file that is not already a wav is
  decoded and written as one, rather than copied under a name nothing could open. The folder
  and the toggles are saved with the project, so a session reopened another day writes where
  it wrote before.
- feat: Quarry now says what key a take is in. **DETECTED**, under the scale quantize controls,
  reads the transcribed notes and names the key, with a number beside it saying how strongly
  they fit. It judges the transcription as it came out of the model, so turning scale quantize
  on cannot make the reading agree with the key you typed in. Material with too little in it to
  call, a drum loop above all, reports no clear key rather than naming one. **Use it** copies
  the detected key into the snap controls. Scale quantize was only ever an instruction; this is
  the reading it looked like it was giving.
- feat: record what the computer is playing. The audio input panel has a new **System Audio**
  driver at the top of the driver list, whose inputs are the machine's playback outputs
  (speakers, headphones, an interface). Pick one and Quarry records everything coming
  out of it: a YouTube video, a browser tab, another app. Windows only, through WASAPI loopback;
  on other systems the driver list is unchanged.
- feat: the standalone app opens ready to record the computer. On its first run it points itself
  at the default playback output, so there is nothing to set up before hitting record. Running
  inside a DAW is unaffected: there the plugin still defaults to the audio the host sends it,
  because recording the host's own output while the plugin is monitored would feed back.

### Changed

- change: the sensitivity knobs offset a threshold read off the take rather than setting one
  outright. They used to map straight onto the decoder as `1 - value`, with no reference to the
  material, which asked you to find the right number by ear with no feedback; published work on
  Basic Pitch finds the difference between 0.5 and 0.6 worth close to 50% relative F1. Quarry now
  measures each posteriorgram's noise floor and fits an Otsu split above it, and the knobs move
  from there. Both default to centred, meaning "use what the audio says".
- change: notes shorter than 125 ms are no longer discarded. That floor was inherited and it is a
  musical filter wearing a noise gate's clothes: it silently deleted every semiquaver at 120 BPM
  along with every trill, grace note and ornament. 75 ms is swept on the bench rather than
  guessed, being the highest note-level F1 that still recovers every reference note.
- change: a note's reported end follows its decay instead of stopping at a global threshold. The
  old rule had no decay model, no release and no pedal, so anything that decays was truncated
  early. Detection is untouched: whether a note exists is still settled against the threshold, and
  only the offset moves.

- ui: Quarry opens on **SAMPLE**. Capturing is the first thing the app does; transcribing is
  something you then do to a capture, reached by handing one over, with **< SAMPLES** as the way
  back. The two page tabs are gone.
- ui: the captures are browsed the way they are stored, walking the dated folders rather than
  flattening every take into one list. Searching still looks everywhere, because not knowing
  which folder a thing is in is the reason anyone searches.
- ui: a capture's row says when it was taken, what the window was showing, and how long it runs.
  The application column said the same thing on every row, and loudness is something you want
  when using a sample rather than when finding one. Both are still in the sidecar.
- ui: the button that sets an application back to full volume appears only when there is an
  application below full volume. A permanent button for a rare problem teaches you to stop
  looking at that corner of the window.
- ui: the whole window moves to Obsidian, the look and feel shared across the OK Studio line,
  so Quarry matches Keys rather than the plugin it was forked from. New wordmark drawn as text
  instead of baked into the background image, new app icon, and a toolbar icon set drawn for
  this window rather than inherited from a light one.
- ui: the audio input panel is now the **SOURCE** strip, docked under the toolbar instead of
  hidden behind a microphone button. What Quarry is about to record is always on screen.
- ui: the piano keyboard down the side of the roll is gone. It was the brightest thing in the
  window, competing with the notes it labelled, and it spent 50 px of width on a pitch ruler.
- ui: transcribed notes are drawn in the app's accent, from deep to bright with amplitude,
  rather than a green to blue to red ramp that read as three categories instead of a scale.
- The plugin is now **Quarry**, part of the OK Studio line. The manufacturer code, plugin code
  and bundle id changed with it (`OKSt`/`Quar`/`com.okstudio.quarry`), so a DAW sees it as a new
  plugin and will need a rescan. Projects saved with the old NeuralNoteVideo build will **not**
  pick Quarry up in its place: those codes are how a host finds a plugin again, so it reports the
  old one as missing and drops the instance, saved state and all. There is no migration, and none
  is possible from inside the plugin. Installing Quarry does not remove an older build, it goes to
  its own folder, so if you still need those projects, keep the old one installed, or open them
  with it once and export the MIDI you want. Otherwise, add Quarry to the track and transcribe
  again.
- Settings and recordings moved with the name, and are **not** migrated. The audio input
  selection now lives under `Quarry/QuarryAudioInput.settings` instead of
  `NeuralNote/NeuralNoteVideoAudioInput.settings`, and recorded audio is written to the `Quarry`
  application-data folder instead of `NeuralNote`. The old folder and its contents are left where
  they are; pick an input again once, and delete the old folder by hand if you want the disk back.

### Removed

- remove: ASIO. Quarry no longer builds with `JUCE_ASIO`, and Steinberg's SDK is out of the tree.
  Windows Audio and DirectSound remain, and neither source anyone actually records from went
  through ASIO: the loopback path is WASAPI, and a transcription tool never spends the round-trip
  latency ASIO exists to cut. Shipping the SDK needs a signed agreement from Steinberg, which is
  a price worth paying for a live instrument and not for this.
- remove: the online description. Quarry opens no socket. A written summary was to have had a
  second rendering through a hosted model, off by default, sending extracted facts and never
  audio. It is cut rather than deferred: nothing could pay for it without either shipping an
  extractable key or standing up a proxy service, the design already treated it as an upgrade
  that every failure path fell back from, and the local paragraph was always the real feature.
  `DESCRIBE_ONLINE` is struck before the parameter ids freeze.

### Fixed

- fix: a dropped stereo file was transcribed from its left channel alone. `resampleBuffer`
  preserves the channel count and the engine is handed one pointer, so the right channel was
  discarded without a word. On a piano recorded with a spaced pair that is not half the level, it
  is the near mic's end of the keyboard. The capture path has always downmixed; only the file path
  did not.
- fix: quiet input could produce notes out of silence. The onset inference divides by the largest
  minimum note-posteriorgram difference in the take, which is zero on anything near-silent or
  heavily gated, and the resulting NaN fails every comparison at the gate that exists to reject
  the frame, so the frame passed. Upstream has the same hole.
- fix: a minor second could not survive decoding. Both decoding passes zeroed the neighbouring
  pitch bins across a note's whole duration to suppress leakage, so clusters, close voicings and
  any two-part writing touching a semitone lost a voice, structurally, every time. A neighbour is
  now zeroed only when it is weak relative to the centre bin: two strong adjacent bins are two
  notes. On the bench's minor-second case, F1 goes 0.667 to 1.000.
- fix: turning a sensitivity knob no longer re-sorts the whole file. The melodia pass sorted one
  record per (frame, pitch) on every parameter change, which on five minutes of audio is 2.27
  million records and about 36 MB, and is why the knobs stalled on long takes. The order cannot
  change between tweaks, so it is established once per take. Same output.
- fix: onsets are no longer pinned to the 11.6 ms frame grid. A parabola through the onset peak
  and its neighbours gives the vertex for three multiplies. Invisible in F1 at a 50 ms tolerance,
  as it must be; mean onset error goes 5.4 ms to 3.7 ms.

- fix: `Notes::convert` honours the note range it is given. `minFrequency` and `maxFrequency`
  were applied as the bounds of one loop, while the melodia pass that follows walks every band
  there is, so asking for 330-1567 Hz still came back with notes an octave and more below the
  floor. basic-pitch clears the posteriorgrams outside the range instead, and now so does Quarry.
  Latent in the app as shipped, which sets neither bound, but it is why the notes test had been
  red: ten of ten cases pass now, where the run used to stop at case five with sixteen events
  against a golden nine. Nothing inside a range moves, so no existing transcription changes.
- fix: the onnxruntime tarball is fetched once per machine rather than once per checkout.
  `OKSTUDIO_ONNXRUNTIME_CACHE` names a directory to keep it in, `OKSTUDIO_ONNXRUNTIME_URL` points
  the fetch at a mirror, and either way the 600 MB download is checked against a pinned SHA-256
  before it is unpacked, and dropped rather than cached if it does not unpack.
- fix: `run.py` recovers from a build tree generated for a different directory. Renaming or
  moving the repo left CMake pointing at a path that no longer existed, and the build failed
  inside MSBuild's regenerate step, nowhere near anything that suggested the cause.
- fix: the Sample page no longer opens with a source selected that nobody picked. The "nothing
  chosen" sentinel was zero, which is also a real process id, so an untouched page showed a row
  as chosen and lit the record button.
- fix: the captures list no longer slices its last row through the middle of the text. Its
  height was whatever the layout had left over rather than a whole number of rows.
- fix: the standalone no longer shows a yellow "audio input is muted to avoid feedback loop"
  banner above the window. It declared a stereo input bus it never read, since recording goes
  through the device picked in the source strip, and JUCE inferred a feedback loop from it.
- fix: text and icons meet WCAG 2.2 contrast on the dark window. The toolbar icons were drawn
  near-black for the old light theme and measured 1.09:1 against the background; several
  components still painted their labels in black, so values like the note range read at
  1.58:1. Icons now clear the 3:1 SC 1.4.11 asks of graphical objects, and text clears the
  4.5:1 SC 1.4.3 asks of text, which is the stricter rule the faintest label tier answers to.
- fix: a disabled row now looks disabled all the way through. The note range names and the
  tempo and time signature boxes paint themselves, so nothing faded them when their row was
  switched off and they read as live controls in a dead panel.
- fix: the export tempo no longer sits on top of the waveform. It kept the position it had
  when the piano keyboard reserved a gutter down the left, so once that went it covered the
  waveform, swallowed clicks meant for the playhead, and lost its own caption.

- Picking an audio input is standalone-only. In a DAW the panel now hides the driver, input and
  channel pickers and records the audio the host sends the plugin, as it always did. An ASIO driver
  serves one client at a time, so a plugin that opened one for itself could take the driver out
  from under the host it is running in.
- **System Audio** inputs are remembered by their Windows endpoint id instead of by their name. Two
  outputs can be called the same thing, and any of them can be renamed, either of which was enough
  to come back to the wrong one.
- A fresh clone builds with just `git`, `cmake` and a compiler. The okstudio kit headers Quarry
  records system audio through are checked in under `ThirdParty/okstudio`, so no checkout of that
  (private) repository is needed alongside this one. See `ThirdParty/okstudio/README.md` for how
  they are kept in sync.

### Notes

- Stopping a recording already starts transcription on its own; no extra click is needed.
