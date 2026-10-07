#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
#
# sampan-send | sampan-recv through a pipe and a file, both codecs.
set -eu
B=${1:-build/host}
T=$B/e2e
mkdir -p "$T"

# PCM is lossless: the received WAV must equal what was sent, byte for byte.
"$B/sampan-send" -c pcm -r 48000 -d 1.5 -n 500 -w "$T/ref.wav" - |
    "$B/sampan-recv" -q -o "$T/pcm.wav" - 2>"$T/pcm.log"
cmp "$T/ref.wav" "$T/pcm.wav"

# Same through a file, then back in as the input of a second pass.
"$B/sampan-send" -c adpcm -r 16000 -d 2 "$T/adpcm.bin"
"$B/sampan-recv" -q -o "$T/adpcm.wav" "$T/adpcm.bin" 2>"$T/adpcm.log"
size=$(wc -c <"$T/adpcm.wav" | tr -d ' ')
[ "$size" -eq $((44 + 16000 * 2 * 2)) ]

# WAV file as input; PCM is lossless again.
"$B/sampan-send" -c pcm -r 44100 -d 0.5 -w "$T/mono.wav" "$T/unused.bin"
"$B/sampan-send" -c pcm -i "$T/mono.wav" - | "$B/sampan-recv" -q -o "$T/again.wav" - 2>/dev/null
cmp "$T/mono.wav" "$T/again.wav"

# Joining late: a jump of thousands of packets (data queued before the port
# was opened) is a resync, not seconds of silence.
{ "$B/sampan-send" -c adpcm -r 16000 -d 1 -
  "$B/sampan-send" -c adpcm -r 16000 -d 1 -S 5000 -; } |
    "$B/sampan-recv" -q -o "$T/late.wav" - 2>"$T/late.log"
size=$(wc -c <"$T/late.wav" | tr -d ' ')
[ "$size" -eq $((44 + 16000 * 2 * 2)) ]
grep -q "lost 0,.*resyncs 1" "$T/late.log"
# INFO packets share the sequence; a stream with them must show no loss.
grep -q "lost 0," "$T/adpcm.log"

grep -q "lost 0, crc errors 0, cobs errors 0" "$T/pcm.log"
grep -q "info: sampan-send" "$T/adpcm.log"
echo "e2e ok"
