# Changelog

All notable changes to NeuralNoteVideo are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- feat: record what the computer is playing. The audio input panel has a new **System Audio**
  driver at the top of the driver list, whose inputs are the machine's playback outputs
  (speakers, headphones, an interface). Pick one and NeuralNoteVideo records everything coming
  out of it: a YouTube video, a browser tab, another app. Windows only, through WASAPI loopback;
  on other systems the driver list is unchanged.
- feat: the standalone app opens ready to record the computer. On its first run it points itself
  at the default playback output, so there is nothing to set up before hitting record. Running
  inside a DAW is unaffected: there the plugin still defaults to the audio the host sends it,
  because recording the host's own output while the plugin is monitored would feed back.

### Changed

- feat: the RECORD button in the audio input panel is now a large 180x72 target, and the panel
  has grown to make room for it. It is the button that matters most, so it is the easiest one
  to hit.

### Notes

- Stopping a recording already starts transcription on its own; no extra click is needed.
