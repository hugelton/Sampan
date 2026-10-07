<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Sampan stream protocol

Version 1. Carried on the USB serial port (CDC-ACM) of the device, or on a
UART on boards without native USB. The reference implementation is
`core/include/sampan/proto.h` and `core/src/proto.c`; `host/sampan-recv.c` is
a complete receiver.

## Framing

Each packet is COBS-encoded and followed by one `0x00` byte. A receiver can
start anywhere: it drops bytes until the next `0x00`, and any frame that fails
to decode or check is dropped on its own. Empty frames (`00 00`) are allowed
as idle filler.

## Packet

All multi-byte fields are little-endian.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | `version << 4 \| type` (version 1) |
| 1 | 1 | flags, 0 |
| 2 | 2 | sequence number, +1 per packet of any type, wraps |
| 4 | n | body |
| 4 + n | 2 | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over bytes 0 .. 3 + n |

Types: 1 audio, 2 MIDI (raw MIDI bytes), 3 info (UTF-8 text).

A jump in the sequence number means packets were lost. A jump much longer
than the audio a receiver buffers is a resync (the receiver joined late or
the sender restarted), not loss.

## Audio body

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | codec: 0 = signed 16-bit PCM, 1 = IMA ADPCM |
| 1 | 1 | channels, 1 or 2 |
| 2 | 4 | sample rate, Hz |
| 6 | 2 | frames (samples per channel), 1 .. 512 |

PCM: interleaved 16-bit samples follow.

ADPCM: first, per channel, the decoder state before this packet's first
sample: predictor (int16), step index (uint8, 0 .. 88), one reserved byte.
Then, per channel in turn, `(frames + 1) / 2` bytes of 4-bit codes, low
nibble first. The tables and the decoding step are standard IMA ADPCM
(checked bit for bit against an independent decoder); since every packet
carries its starting state, a lost packet costs only its own audio.

Wire rate for mono at 256 frames per packet: 48 kHz PCM about 99 kB/s, 48 kHz
ADPCM about 28 kB/s, 16 kHz ADPCM about 9 kB/s.

## Info body

A line of text. The device sends one each second with the version, what it
found on the board, levels, drop counters and the Bluetooth state.

## Restart into the loader

Setting the serial port to 1200 baud and dropping DTR (opening at 1200 baud
and closing) restarts the device into its ROM loader for flashing.
