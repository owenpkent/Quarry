# Changelog

All notable changes to Quarry are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- feat: keep a take without opening a dialog. A **SAVE TO** bar along the bottom writes the
  recorded audio and the transcription to a folder picked once, with a toggle for each format.
  The name of the next take is shown before you commit to it, and nothing is ever overwritten.
  The folder and the toggles are saved with the project, so a session reopened another day
  writes where it wrote before.
- feat: Quarry now says what key a take is in. **DETECTED**, under the scale quantize controls,
  reads the transcribed notes and names the key, with a number beside it saying how strongly
  they fit: tonal material scores around 0.9, percussive material near zero. **Use it** copies
  that key into the snap controls. Scale quantize was only ever an instruction; this is the
  reading it looked like it was giving.

### Changed

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

### Fixed

- fix: the standalone no longer shows a yellow "audio input is muted to avoid feedback loop"
  banner above the window. It declared a stereo input bus it never read, since recording goes
  through the device picked in the source strip, and JUCE inferred a feedback loop from it.
- fix: text and icons meet WCAG 2.2 contrast on the dark window. The toolbar icons were drawn
  near-black for the old light theme and measured 1.09:1 against the background; several
  components still painted their labels in black, so values like the note range read at
  1.58:1. Both are well clear of the 3:1 the criterion asks for now.

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
- feat: the RECORD button in the audio input panel is now a large 180x72 target, and the panel
  has grown to make room for it. It is the button that matters most, so it is the easiest one
  to hit.

### Fixed

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
