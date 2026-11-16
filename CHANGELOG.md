# Changelog

All notable changes to Beiwagen. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Items
checked on an FM-1 say so; the rest is built but not yet tested on hardware.

## [Unreleased]

## [0.2] - 2026-11-16

The first Beiwagen release (0.1 is FM1_AudioBase's, the firmware Beiwagen is forked from).

### Added

- The FM-1 as USB sidecar for the OP-1 Field: the OP-1 streams its four tape tracks (44.1 kHz, USB mode 8) and records
  the FM-1's return. Streaming checked on an FM-1 with an OP-1 Field 1.7.3.
- The FM-1's keys play the OP-1 over USB-MIDI. Checked on an FM-1.
- Effect chain, HOME page with track tiles, USB diagnostics and scope.
- Mixdown: per-track inserts and sends to a shared delay and reverb; REC finds the return track and mutes it.
- Sequencer and arpeggiator; presets and patterns stored on the keys (hold SAVE + key, recall with SEL + key).
- The keys' MIDI channel and velocity are kept; buttons without a function stay unlit.
- System settings (hold HOME, press EDIT), stored in their own flash sector.
- The update loader shows Elektro Kabelmaier's update screen while it writes.

[Unreleased]: https://github.com/DPangerl/beiwagen/compare/v0.2...HEAD
[0.2]: https://github.com/DPangerl/beiwagen/releases/tag/v0.2
