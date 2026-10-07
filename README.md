<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Sampan

[![Licence: GPL-3.0-only](https://img.shields.io/badge/licence-GPL--3.0--only-blue.svg)](LICENSE)

**TL;DR:** Sampan 0.1 — early development, tested on one board. Plug it in
and your computer sees a USB microphone, a USB-MIDI port and a serial port,
phones or tablets can reach it as a Bluetooth LE MIDI device, and the button
records short samples you can play from MIDI. There is no
packaged release yet: build and flash it from source ([BUILDING.md](BUILDING.md)).
Flashing is at your own risk; the board's ROM loader always stays available,
so you can go back to any other firmware.

Microphone, sampler, MIDI bridge and audio stream firmware for small ESP32-S3 boards.
Please report what you find in Issues.

- Build and flash: [BUILDING.md](BUILDING.md)
- Stream format: [docs/protocol.md](docs/protocol.md)

## Features

- **USB microphone:** UAC2, 48 kHz, 16-bit, mono; class compliant, no driver
  on macOS; the host's input volume and mute act on the digital gain.
- **Audio stream:** the same microphone signal on a USB serial port as framed
  packets (16-bit PCM or 4:1 IMA ADPCM), with a status line once a second;
  `sampan-recv` saves it as WAV.
- **MIDI:** USB-MIDI and Bluetooth LE MIDI at once; the button sends Control
  Change on both.
- **MIDI bridge:** what arrives over Bluetooth goes out on USB and the other
  way round, so a USB host can be played from a phone or tablet. Under 1 ms
  inside Sampan on either path.
- **Bluetooth LE MIDI:** standard MIDI-over-BLE service, MTU up to 250,
  bonding with the keys kept across restarts.
- **Sampler:** hold the button to record (up to 10 s); the take is trimmed,
  levelled and faded, kept on the board's flash as WAV and played on the
  speaker. MIDI note on plays it at pitch (60 = as recorded, velocity sets
  the level).
- **Input:** DC cut (20 Hz), analogue and digital gain with a smooth ramp,
  peak and RMS level meter.
- **Screen:** triggered waveform and level meter, five one-colour palettes.
- **Flashing without buttons:** opening the serial port at 1200 baud and
  closing it restarts the board into its ROM loader (`flash.sh` does this).

## Controls

One button:

| Gesture | On the board | MIDI sent (channel 1) |
| --- | --- | --- |
| press / release | — | CC 80: 127 on press, 0 on release |
| click | play the take | CC 102: 127 then 0 |
| double click | next palette | CC 106: 127 then 0 |
| long press (0.6 s) | record until released | CC 110: 127 then 0 |

While recording, the strip under the waveform turns red.

MIDI received on USB or Bluetooth, channel 1:

| Message | Does |
| --- | --- |
| note on | play the take at that pitch (60 = as recorded) |
| CC 85 | 127 starts recording, 0 stops |
| CC 86 | play the take |
| CC 87 | playback volume |
| CC 88 | microphone gain, 0 to 24 dB |

## Hardware

Runs on the AtomS3R with the Atomic Echo Base (ESP32-S3, 8 MB flash, 8 MB
PSRAM, ES8311 codec with a MEMS microphone). Both 128 × 128 panels that have
shipped on this board are detected at start-up.

## Layout

| Path | What |
| --- | --- |
| `core/` | portable C: stream protocol, IMA ADPCM, ring buffer, button gestures, MIDI (parser, USB-MIDI, BLE MIDI), waveform display, DC cut / gain / meter, sampler (trim, level, fade, pitched playback, WAV) |
| `host/` | computer tools: `sampan-recv` (stream to WAV), `sampan-send` (test streams), `sampan-scope` (screen previews as PNG) |
| `tests/` | host test suites (run under AddressSanitizer and UBSan) and an end-to-end pipe test |
| `firmware/atoms3r/` | ESP-IDF firmware for the board: codec, panel, USB composite device, BLE MIDI, sampler |
| `firmware/components/` | the portable core as an ESP-IDF component |
| `tools/` | device checks on macOS: MIDI listener, BLE bridge and latency tests |
| `docs/` | stream protocol |

## If the board does not start

- Symptoms: no "Sampan" audio or MIDI device, no `usbmodem…` serial port.
- First try another cable and USB port, and close programs that hold the
  serial port.
- If the board crashes twice in a row it starts in a safe mode (no speaker,
  no sampler) so USB still comes up and you can flash it again.
- Put the board into its ROM loader: hold the side button for about 2 s until
  the green LED inside lights, then release. It shows up as a USB serial/JTAG
  device; run `./flash.sh` again ([BUILDING.md](BUILDING.md)). The ROM loader
  cannot be overwritten, so this always works.

## Support

Issues are for reproducible bugs (one per issue). Ideas and requests go to
Discussions. Pull requests are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

- **[Hügelton Instruments](https://hugelton.com)** (Leo Kuroshita,
  [@kurogedelic](https://github.com/kurogedelic)): Sampan, its host tools and
  tests; the screen palettes shared with Hügelton's other firmware.
- [TinyUSB](https://github.com/hathach/tinyusb), Ha Thach and contributors,
  MIT.
- [ESP-IDF](https://github.com/espressif/esp-idf) with the NimBLE host,
  Espressif Systems, Apache-2.0.
- Panel and codec register values from
  [LovyanGFX](https://github.com/lovyan03/LovyanGFX) (lovyan03, FreeBSD,
  [LICENSES/LovyanGFX-FreeBSD.txt](LICENSES/LovyanGFX-FreeBSD.txt)),
  [M5GFX](https://github.com/m5stack/M5GFX) and
  [M5Unified](https://github.com/m5stack/M5Unified) (M5Stack, MIT,
  [LICENSES/](LICENSES/)).

## Licence

Free software: [GPL-3.0-only](LICENSE). The SDK and USB stack linked into the
firmware and the register values taken from other projects keep their own
licences ([LICENSES/](LICENSES/)); details in [LICENSING.md](LICENSING.md).

M5Stack, AtomS3R and Atomic Echo Base are trademarks of M5Stack Technology;
ESP32-S3 is a trademark of Espressif Systems; the Bluetooth word mark is owned
by Bluetooth SIG. Sampan is not affiliated with or endorsed by them.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
