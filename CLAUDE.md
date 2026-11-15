# Beiwagen

FM-1 firmware (M-VAVE FM-1, JieLi AC79, pi32v2) that makes the FM-1 a USB sidecar for the OP-1 Field: the OP-1 is the
USB host, plays its tape tracks into the FM-1 (44.1 kHz only, USB mode 8 = tracks 1-4 stereo), the FM-1 effects one
pair and sends it back as the OP-1's USB input. A fork of stevexfer's FM1_AudioBase (MOOD_FM1 → GATOR → Felucca,
GPL-3.0-only). Upstream remote: `upstream` (stevexfer/FM1_AudioBase); ours: `origin` (DPangerl/beiwagen). Public
repo: everything in English. The app is `firmware/apps/beiwagen/` (default); `audiobase` and `demo` stay as upstream
has them (easy merges).

Proven on hardware (2026-11-15, OP-1 Field 1.7.3): the FM-1 is full-speed USB only; the OP-1 as host streams to it at
44.1 kHz only (48 kHz: a burst, then nothing); 10-ch packets need the EP3 read every TIMER5 tick (0.6 ms on the
wire); the OP-1 puts its USB input on the selected track, together with live playing, so live effects loop: effect
track A, record onto track B. The keys play the OP-1 over USB-MIDI (OP-1 MIDI set to receive notes). Never claim
untested features (effects, sequencer) as working.

- Beiwagen app layout: `app.c` (sections, audio), `bw_ui.h` (page kit: header, body strip, 4 knob rows), `bw_home.h`
  (HOME: overview, USB diagnostics, scope), `bw_keys.h` (keys → USB-MIDI); unity-included headers, static functions.
- Product plan (control map): GLO through-effects, FX bypass, SEQ sequencer, ARP arpeggiator, PLAY transport, HOME
  overview + setup, LFO/ENV modulation routed to the effect chain (later the synth), SELECT pages, KNOB1-4 page
  params, ALGORITHM source (OP-1 pair), PRESETS presets/patterns, MASTER level. SEL/EDIT/SAVE/REC unassigned.
- GPL-3.0-only, like MOOD_FM1 and GATOR: never copy its code into a clean-room project (an agent that has read this
  code does not edit one).
- Unity build: `firmware/sys/fw.c` includes every `.c`; include order matters. Apps live in `firmware/apps/NAME/`
  (`app.c`, optional `*.c` / `*.cc` built separately with the FPU, `build.txt`, `third_party.txt`, `name.txt`,
  `logo.png`); `--app NAME` selects one (default `beiwagen`; `demo` is the small synth example).
- C in the unity build is built without FPU (as ORBIT); the app's other `*.c` and `*.cc` with `-mcpu=r3 -mfprev1`.
  Cross with ints/pointers only (`apps/beiwagen/bw_fw.h`). No libc on the target: no `NULL`, use 0. No static constructors (build fails on `.init_array`).
- `build.txt`: `fs 44|48` (the app's codec rate; beiwagen: 44), `define FW_USB_PLAY_CH=N`, `define FW_USB_PLAY=1` (the computer plays to the FM-1), `define FELUCCA_UAC=1` (it records the FM-1),
  `define FW_USB_PRODUCT="FM-1"` (must contain "FM-1": the installers look for it), `c PATH`, `include DIR`, `small FILE`.
- USB audio (`sys/usb.c`): playback at the codec's rate family, FW_USB_PLAY_CH channels (one pair heard), adaptive, resampled to the codec clock; capture at the codec's rate,
  async, the codec's samples bit for bit (`uac_tap` in `sys/audio.c`, after the app's volume). Audio streaming
  interfaces come before MIDI in the AC header (macOS 15 and older need that). Both are serviced from TIMER5 (USB poll 2 kHz, the
  playback endpoint every 10 kHz tick), nested into the render (`sys/main.c`).
- The sample rate lives in `firmware/sys/fw_rate.h` (selected per app by build.txt `fs`); beiwagen runs the 44.1 kHz
  family (44,117.6 Hz). `FW_FS` is the rounded integer, `FW_FS_HZ` the exact float.
- `firmware/hal/`, `firmware/loader/`, `sys/ota.c`, `sys/recovery.c`, `sys/storage.c`, `sys/midi_uart.c` come from
  ORBIT and are hardware-verified there: change them only with a reason. The loader must stay byte-identical unless
  deliberately changed (it is what makes reflashing safe). The packer already stores the SDK SPL BANKCB/ENC-encoded.
- Register access only in `hal/`; `tools/build.py` fails on MMIO literals, volatile casts or inline asm in `sys/`, `apps/`, `loader/`.
- `app_audio` runs in the ALNK0 ISR (256 frames, 5.3 ms); TIMER5 (input, USB) outranks it. No float in the unity
  build, no malloc. The app must drain `fw_midi_read` every frame (a full ring holds back the installer's SysEx).
- Target build: `PYTHON=.venv/bin/python ./build.sh [--app NAME]` (Docker on macOS; SDK files vendored in `tools/sdk/`).
  Host check: `.venv/bin/python tools/sim.py` (ASan/UBSan, fatal; `usbtone` / `usbrec` fake the USB audio). Host
  results say nothing about timing. Emulator end to end: FM-1Ception `fm1-live` (BUILDING.md). Update paths:
  `tests/update_tests.sh build/audiobase.fwsc build/app.bin` (STOCK_V15 / STOCK_V16: M-VAVE's official V15 / V16 `.fwsc`).
- `sys/audio.c` acknowledges the ALNK half before rendering (ChoralRoot 0.18, heard on hardware); OTA sets
  `fw_audio_hold` so the app does not render; `fm1_cstart` raises the supplies to stock (SLOOP 2.7). Keep these.
- Web installer: `web/` (fm1ota.js / fm1pkg.js are SLOOP 2.7's; fm1ota.js's FM1_PORT also takes the loader's macOS names). `web/make_site.py [--release
  build/release-X.Y]` -> `build/site`; `node web/test_web.mjs PKG` is the protocol test.
- Releases: by hand (BUILDING.md): `tools/release.py X.Y`, tag `vX.Y` (one digit each, identity FM-1_14923XY; the loader
  keeps `ota-FM-1_900`), `gh release create`. Actions are disabled on origin (they cost money): never re-enable or trigger them.
  The Firmware Hub card is `fmhub.json`. Release notes in `docs/releases/X.Y.md`. `LICENSE`, `LICENSING.md`, `LICENSES/` travel with every package.
- New files carry `SPDX-License-Identifier: GPL-3.0-only`; keep the existing copyright headers. A file changed for
  Beiwagen gets a line "Modified for Beiwagen, 2026 (Daniel Pangerl): what". A new third-party part goes into LICENSING.md,
  README's licence table, LICENSES/ and the installer's Licences section.
- The splash stays `FW_SPLASH_MS` (main.c) from drawing it; `app_init` runs meanwhile and must not draw.
- Never claim hardware testing that did not happen.
