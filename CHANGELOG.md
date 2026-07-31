# Changelog

All notable changes to Quarry are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
