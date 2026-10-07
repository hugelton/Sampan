#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
#
# Flash without touching the board: if Sampan is running, open its serial port
# at 1200 baud and close it (the firmware restarts into the ROM loader), then
# write build/ and start the app with a watchdog reset.
# Usage: ./flash.sh   (after idf.py build, with the ESP-IDF environment loaded)
set -eu
cd "$(dirname "$0")"

rom_port() { ls /dev/cu.usbmodem* 2>/dev/null | grep -E 'usbmodem[0-9]+$' | grep -v 112304 || true; }
app_port() { ls /dev/cu.usbmodem* 2>/dev/null | grep -E 'usbmodem[0-9A-F]{12}[0-9]$' || true; }

p=$(app_port | head -1)
if [ -n "$p" ]; then
    echo "Sampan on $p: asking it to restart into the loader"
    python3 -c "
import serial, time
s = serial.Serial('$p', 1200); time.sleep(0.2); s.dtr = False; s.close()"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.5
        [ -n "$(rom_port)" ] && break
    done
fi

p=$(rom_port | head -1)
if [ -z "$p" ]; then
    echo "No loader port. Hold the side button about 2 s until the green LED, then run again." >&2
    exit 1
fi
echo "loader on $p"
cd build
esptool.py --chip esp32s3 -p "$p" --before no_reset --after watchdog_reset write_flash @flash_args
