# Changelog

All notable changes to Quarry are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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

### Fixed

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
