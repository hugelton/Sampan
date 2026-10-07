<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Licensing

## What is GPL-3.0-only

Everything in this repository unless listed below: `core/`, `host/`,
`tests/`, `firmware/`, `tools/`, `docs/`, the build files. Full text in
[LICENSE](LICENSE). Every source file starts with an SPDX line and
"Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments".

## Hügelton Instruments' own work

Sampan itself, its host tools and tests, and the screen palettes it shares
with Hügelton Instruments' other firmware. All GPL-3.0-only here.

## Third-party material

Only register values, pin numbers and probe IDs are taken from these
projects; no library code is copied.

| Source | Used in | Licence | Text |
| --- | --- | --- | --- |
| LovyanGFX (panel init tables, which carry Adafruit lineage) | `firmware/atoms3r/main/lcd.c` | FreeBSD (BSD-2-Clause) and the BSD notices it includes | [LICENSES/LovyanGFX-FreeBSD.txt](LICENSES/LovyanGFX-FreeBSD.txt) |
| M5GFX (panel pins, ID values, backlight driver set-up) | `firmware/atoms3r/main/lcd.c`, `board.c` | MIT | [LICENSES/M5GFX-MIT.txt](LICENSES/M5GFX-MIT.txt) |
| M5Unified (codec clock and microphone register values) | `firmware/atoms3r/main/es8311.c` | MIT | [LICENSES/M5Unified-MIT.txt](LICENSES/M5Unified-MIT.txt) |

## SDK and USB stack

Linked into firmware builds, not stored here:

- ESP-IDF, including the NimBLE host: Apache-2.0, with its own third-party
  notices.
- TinyUSB (fetched by the ESP-IDF component manager): MIT.

Firmware binaries, when distributed, ship with these licence texts and
notices.

## No vendor material

No vendor firmware, images or binaries are in this repository.

## Contributions

Contributions are accepted under GPL-3.0-only and credited in the README.

## Trademarks

M5Stack, AtomS3R and Atomic Echo Base are trademarks of M5Stack Technology;
ESP32-S3 is a trademark of Espressif Systems; the Bluetooth word mark is owned
by Bluetooth SIG. Sampan is not affiliated with or endorsed by them. The names
are used only to say which hardware it runs on.

## Radio

Sampan uses the board's Bluetooth LE radio with the SDK's default transmit
settings and the board's own antenna, as certified by the board maker. Use
it only where the board is certified for radio use.
