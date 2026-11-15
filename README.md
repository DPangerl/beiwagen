<h1 align="center">Beiwagen</h1>

<p align="center">
  The M-VAVE FM-1 as a USB sidecar for the teenage engineering OP-1 Field<br>
  <a href="BUILDING.md">Building</a> ·
  <a href="docs/DEVELOPING.md">Developing</a>
</p>

Beiwagen is free firmware for the M-VAVE FM-1 that works together with an OP-1 Field, and only with one. The FM-1
plugs into the OP-1 with one USB cable; the OP-1, as the USB host, sends its tape tracks to the FM-1 and records what
comes back.

**Works now** (tested on an FM-1 with an OP-1 Field, firmware 1.7.3):

- **OP-1 → FM-1:** with the OP-1 in USB mode 8, its four tape tracks arrive as four stereo pairs, 44.1 kHz.
- **FM-1 → OP-1:** the pair chosen with KNOB1 plays on the FM-1 and goes back to the OP-1 as its USB input, to be
  recorded onto another tape track.
- **Keys → OP-1:** the FM-1's keys play the OP-1 over USB-MIDI (OCT−/+ for octaves; channel and velocity on HOME).
  On the OP-1, set its MIDI to receive notes, not only send them.
- **Diagnostics** (HOME, second page): the stream's counters, a meter per channel, the host's last USB requests.

**Built, not yet tested on an FM-1:** effects on the chosen track: distortion, filter, chorus, delay and reverb in
series, a page each (GLO, then SELECT), GLO again switches the page's effect on or off, FX bypasses them all.

**Planned:** modulation (LFO, ENV), a sequencer and arpeggiator, presets, and later synths of its own.

The OP-1 puts its USB input on the selected track, together with what is played live, so a track is effected and
recorded onto *another* track: select track 2, choose track 1 on the FM-1, play the tape and record.

> [!WARNING]
> **Installing custom firmware is at your own risk.** Beiwagen is unofficial firmware, provided as is, without any
> warranty (GPL-3.0 sections 15 and 16). It is not made, endorsed or supported by M-VAVE or teenage engineering. A
> failed or interrupted install can leave the FM-1 unable to start until it is recovered with a USB tool. You can
> return to the official firmware at any time.

## Build

See **[BUILDING.md](BUILDING.md)**: the toolchain, `./build.sh`, the host simulator and tests, the emulator, and how
a release is made. [docs/DEVELOPING.md](docs/DEVELOPING.md) describes the platform and its app API.

## Credits

Beiwagen is a fork of [FM1_AudioBase](https://github.com/stevexfer/FM1_AudioBase) by stevexfer, itself a fork of
MOOD_FM1, the FM-1 port of MOOD, which was built on [GATOR](https://github.com/npi3pak/fm1-gator) by npi3pak. It
stands on the work of:

- **stevexfer**: [FM1_AudioBase](https://github.com/stevexfer/FM1_AudioBase), the duplex USB audio base and its
  visualizer.

- **Leo Kuroshita, Hügelton Instruments**: [Felucca](https://github.com/hugelton/Felucca), the FM-1 platform: HAL,
  update loader, USB-MIDI, USB audio, OTA, storage and installers (GPL-3.0).
- **npi3pak**: [GATOR](https://github.com/npi3pak/fm1-gator), the platform cut down and its release tooling.
- **dspaudio**: [ORBIT](https://github.com/dspaudio/orbit), the platform GATOR was cut down from.
- **isod89**: [SLOOP](https://github.com/isod89/sloop-fm1), the web install protocol and the stock supply levels.
- **Quixotic7**: [ChoralRoot](https://github.com/Quixotic7/ChoralRootFM1), the audio interrupt fix and the release
  layout; [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).
- **Charles Vestal**: [AMB-1](https://github.com/charlesvestal/fm1-amb), which showed the FPU in use on the FM-1.
- **Rasmus Andersson and the Inter Project Authors**: the Inter font (SIL OFL 1.1).
- **Dimitar Toshkov Zhekov**: the Terminus font (SIL OFL 1.1).
- **JieLi Technology**: three AC79 SDK files in every package (Apache-2.0).

## License

Beiwagen is free software under the [GNU General Public License v3.0 only](LICENSE), provided without any
warranty. The parts it is built from keep their licences and copyright notices:

| Part | Author | Licence | Text |
|---|---|---|---|
| Beiwagen (the beiwagen app, its USB playback changes) | Daniel Pangerl | GPL-3.0-only | [LICENSE](LICENSE) |
| FM1_AudioBase, MOOD_FM1's visualizer, GATOR (the app, web installer, release tooling) | stevexfer; npi3pak | GPL-3.0-only | [LICENSE](LICENSE) |
| Felucca platform: HAL, update loader, USB-MIDI, USB audio, OTA, storage, tools (through ORBIT and SLOOP, modified) | Leo Kuroshita, Hügelton Instruments | GPL-3.0-only | [LICENSE](LICENSE) |
| Web install protocol `fm1ota.js`, `fm1pkg.js` (from SLOOP 2.7; `fm1ota.js` finds the loader's macOS port names too) | Leo Kuroshita, Hügelton Instruments; isod89 | GPL-3.0-only | [LICENSE](LICENSE) |
| Inter font (rasterised into the firmware) | The Inter Project Authors | SIL OFL 1.1 | [LICENSES/OFL-Inter.txt](LICENSES/OFL-Inter.txt) |
| Terminus font (rasterised into the firmware) | Dimitar Toshkov Zhekov | SIL OFL 1.1 | [LICENSES/OFL-Terminus.txt](LICENSES/OFL-Terminus.txt) |
| Three AC79 SDK files in every package (`tools/sdk`, unmodified) | JieLi Technology | Apache-2.0 | [LICENSES/Apache-2.0-JieLi-SDK.txt](LICENSES/Apache-2.0-JieLi-SDK.txt) |

Every release and the web installer carry these licence texts next to the packages, and each release's complete
source is its tag in this repository. [LICENSING.md](LICENSING.md) has the details.

"M-VAVE" and "FM-1" are trademarks of their respective owners; "teenage engineering" and "OP-1" are trademarks of
teenage engineering. Beiwagen is not affiliated with, endorsed by or supported by them.
