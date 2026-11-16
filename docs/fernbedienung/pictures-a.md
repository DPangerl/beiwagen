<!-- SPDX-License-Identifier: CC0-1.0 -->
# Pictures, part A: sound sources and effects

What the app sends in PICTURE (`20`) and LIVE (`21`) for oscillator, wavetable, FM, string, grain, filter, EQ,
compressor, distortion and chorus. The FM-1 draws Steckfeld's display for each from these values
(`firmware/apps/fernbedienung/fb_pics_a.h`). Field names are Steckfeld's (`ModuleSettings`, `App/Model`).

## Conventions

- **pos**: where the value sits on its encoder's sweep, the way the programmer's ring shows it:
  `n = control.normalized(value)` from `ToolControls.swift`, clamped to 0...1, sent as `round(n * 16383)`.
  Linear: `n = (value - lo) / (hi - lo)`. Log: `n = ln(value / lo) / ln(hi / lo)`. The range and whether it is log
  are given per field below; they are the ones in `toolGroups(for:)`.
- **index**: a choice sent as its number, e.g. an enum's `rawValue` (0, 1, 2, ...).
- **flag**: 0 or 1.
- Array bytes are 7-bit (0...127).
- Send PICTURE when the module is shown and whenever one of its fields below changes. Send every value listed,
  `n` = the number of values in the table.
- **LIVE** is the ghost Steckfeld draws over the picture while cables modulate the module
  (`model.isModulated(module)`): the same fields as the engine plays them now, `model.live(module, value, target)`,
  in the same mapping, clamped to 0...16383. Send all of the kind's LIVE values (unmodulated ones as set), up to
  30 times a second, while the module is modulated. When the modulation stops, send LIVE with `n` = 0: the ghost
  goes. A kind with no LIVE row has no ghost.

## Oscillator (kind 2)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `waveform` | index, `SynthWaveform.rawValue`: 0 sine, 1 triangle, 2 saw, 3 square, 5 noise |
| 2 | v1 | `tune` | pos, -24...24 semitones, linear |
| 3 | v2 | `level` | pos, 0...1 |
| 4 | v3 | `analog.width` | pos, 0.05...0.95, linear (only the square uses it; send it anyway) |

Array: none. LIVE (3): `tune` through `.oscPitch`, `level` through `.oscLevel`, `analog.width` through `.oscWidth`.

## Wavetable (kind 3)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `wavetable` | index, `SynthWavetable.rawValue` |
| 2 | v1 | `wavePosition` | pos, 0...1 |
| 3 | v2 | `waveWarp` | pos, 0...1 |
| 4 | v3 | `level` | pos, 0...1 |

Array (512 bytes): the table's 8 frames, unwarped, 64 samples each, frame after frame (frame 0 first). Frame `f`:

```swift
var s = [Float](repeating: 0, count: 64)
synth_wavetable_preview(table, Float(f) / 7, 0, &s, 64)          // f = 0...7
bytes += s.map { UInt8(min(max((64 + 63 * $0).rounded(), 0), 127)) }
```

The FM-1 blends the frames and applies warp itself, so position and warp move without a new array; resend the
array when the table changes (every PICTURE carries it). LIVE (2): `wavePosition` through `.wavetablePosition`,
`waveWarp` through `.wavetableWarp`.

## FM (kind 5)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `fmAlgorithm` | index 0...7 (`FMAlgorithm.table`) |
| 2 | v1 | `fmIndex` | pos, 0...1 |
| 3 | v2 | `fmFeedback` | pos, 0...1 |
| 4 | v3 | `level` | pos, 0...1 |
| 5 | v4...v7 | `fmRatios[0...3]` | `round(ratio * 4)`, 1...64 (0.25...16 in quarters) |
| 6 | v8...v11 | `fmAmounts[0...3]` | pos, 0...1 |

Array: none. LIVE (3): `fmIndex` through `.fmIndex`, `fmFeedback` through `.fmFeedback`, `level` through `.oscLevel`.

## String (kind 4)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `string.position` | pos, 0...0.5, linear |
| 2 | v1 | `string.pick` | pos, 0...1 |
| 3 | v2 | `level` | pos, 0...1 |

Array: none. LIVE (1): `level` through `.oscLevel`.

## Grain (kind 15)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `grain.position` | pos, 0...1 |
| 2 | v1 | `grain.spray` | pos, 0...1 |
| 3 | v2 | `grain.freeze` | flag |
| 4 | v3 | `grain.size` | pos, 0.01...1 s, log |
| 5 | v4 | `grain.pitch` | pos, -24...24 semitones, linear |
| 6 | v5 | `grain.mix` | pos, 0...1 |

Array, from `model.grainView(of: module)` (`GrainView`):

| # | Bytes | What |
|---|---|---|
| 1 | 0...127 | `peaks`, oldest first (`SYNTH_GRAIN_BINS` = 128): `round(min(max(peak, 0), 1) * 127)` |
| 2 | 128... | 3 bytes per grain playing, up to 32: `p = round(clamp(place, 0, 1) * 16383)` as `p >> 7`, `p & 127`, then `round(clamp(gain, 0, 1) * 127)` |

Length 128 + 3 × grains. The buffer and the grains change all the time: send PICTURE up to 30 times a second while
the grain module is shown (as Steckfeld redraws it). LIVE (1): `grain.position` through `.grainPosition`.

## Filter (kind 6)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `filterMode` | index, `SynthFilterMode.rawValue`: 0 LP, 1 HP, 2 BP, 3 notch, 4 morph |
| 2 | v1 | `cutoff` | pos, 20...18000 Hz, log |
| 3 | v2 | `resonance` | pos, 0...1 |
| 4 | v3 | `filterSteep` | flag (24 dB) |
| 5 | v4 | `filterMorph` | pos, 0...1 |

Array: none. LIVE (3): `cutoff` through `.filterCutoff`, `resonance` through `.filterResonance`, `filterMorph`
through `.filterMorph`.

## EQ (kind 13)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `eq.lowGain` | pos, -18...18 dB, linear |
| 2 | v1 | `eq.midGain` | pos, -18...18 dB, linear |
| 3 | v2 | `eq.highGain` | pos, -18...18 dB, linear |
| 4 | v3 | `eq.lowFrequency` | pos, 20...500 Hz, log |
| 5 | v4 | `eq.midFrequency` | pos, 100...8000 Hz, log |
| 6 | v5 | `eq.highFrequency` | pos, 1000...18000 Hz, log |
| 7 | v6 | `eq.midQ` | pos, 0.3...8, log |

Array: none. LIVE (1): `eq.midFrequency` through `.eqFrequency`.

## Compressor (kind 14)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `compressor.threshold` | pos, -48...0 dB, linear |
| 2 | v1 | `compressor.ratio` | pos, 1...20, log |
| 3 | v2 | `compressor.makeup` | pos, 0...24 dB, linear |

Array: none. No LIVE.

## Distortion (kind 11)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `effects.distortionMode` | index, `SynthDistortionMode.rawValue`: 0 clip, 1 fold, 2 crush |
| 2 | v1 | `effects.distortionDrive` | pos, 0...1 |

Array: none. LIVE (1): `effects.distortionDrive` through `.distortionDrive`.

## Chorus (kind 12)

| # | v | Field | Mapping |
|---|---|---|---|
| 1 | v0 | `effects.chorusRate` | pos, 0.05...10 Hz, log |
| 2 | v1 | `effects.chorusDepth` | pos, 0...1 |

Array: none. The FM-1 sweeps the taps at the rate itself; nothing to send for that. LIVE (1):
`effects.chorusDepth` through `.chorusDepth`.
