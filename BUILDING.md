<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Building Sampan

## Prerequisites (macOS)

- Xcode command line tools (`cc`, `make`, `swiftc`).
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5.5 with the
  ESP32-S3 tools:

```
git clone -b v5.5.5 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.5
~/esp/esp-idf-v5.5.5/install.sh esp32s3
```

The first firmware build fetches TinyUSB through the ESP-IDF component
manager.

## Host tools and tests

```
make test     # core suites under ASan/UBSan, then the end-to-end pipe test
make tools    # build/host/sampan-recv, sampan-send, sampan-scope
```

## Firmware

```
. ~/esp/esp-idf-v5.5.5/export.sh
cd firmware/atoms3r
idf.py build
```

## Install

With Sampan running (its `usbmodem…` serial port present):

```
./flash.sh
```

`flash.sh` opens the serial port at 1200 baud and closes it, which restarts
the board into its ROM loader, writes `build/` and starts the new firmware.
If no Sampan port is there, hold the side button for about 2 s until the green
LED lights, then run `./flash.sh` again.

## Checks on the device

- Stream: `build/host/sampan-recv -o take.wav /dev/cu.usbmodem…` prints the
  status line and writes WAV.
- Microphone: choose **Sampan** as the input in System Settings → Sound, or
  record with any audio program.
- MIDI and BLE: see [tools/README-tests.md](tools/README-tests.md).
