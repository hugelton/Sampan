/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * MIDI plumbing shared by USB-MIDI, BLE MIDI and the Sampan stream:
 *   - a byte-stream parser (running status, realtime, SysEx in 3-byte chunks)
 *   - USB-MIDI 1.0 event packets, both ways
 *   - BLE MIDI packets, both ways
 *   - button gestures -> Control Change
 */
#ifndef SAMPAN_MIDI_H
#define SAMPAN_MIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One complete message, or a piece of SysEx (up to 3 raw bytes, F0/F7 kept). */
struct sampan_midi_msg {
    uint8_t len;
    uint8_t b[3];
    bool sysex;     /* b[] is SysEx data */
    bool sysex_end; /* this piece ends the SysEx (F7 or cut short by a status) */
};

/* Length of a message by its status byte (including the status), 0 if not a
 * valid start (data byte, F0, F7, F4, F5, F9, FD). */
uint8_t sampan_midi_msg_len(uint8_t status);

struct sampan_midi_parser {
    uint8_t status; /* message being assembled; kept as running status for channel messages */
    uint8_t data[2];
    uint8_t have, need;
    bool in_sysex;
    uint8_t sx[3];
    uint8_t sx_len;
};

void sampan_midi_parser_init(struct sampan_midi_parser *p);
/* Feed one byte. Returns the number of messages written to out (0..2). */
int sampan_midi_parse(struct sampan_midi_parser *p, uint8_t byte, struct sampan_midi_msg out[2]);

/* USB-MIDI 1.0: 4-byte event packet. Returns false for an empty message. */
bool sampan_usbmidi_pack(uint8_t cable, const struct sampan_midi_msg *m, uint8_t pkt[4]);
/* Raw MIDI bytes carried by a packet (0..3), copied to out. */
int sampan_usbmidi_unpack(const uint8_t pkt[4], uint8_t out[3]);

/* BLE MIDI. Timestamps are milliseconds, 13 bits on the air. */
#define SAMPAN_BLEMIDI_MAX 247 /* largest payload we build (ATT MTU 250 - 3) */

struct sampan_blemidi_tx {
    uint8_t buf[SAMPAN_BLEMIDI_MAX];
    size_t len;
    size_t max; /* payload limit: ATT MTU - 3, clamped to 8..SAMPAN_BLEMIDI_MAX */
    uint8_t ts_high;
};

void sampan_blemidi_tx_init(struct sampan_blemidi_tx *t, size_t max_payload);
/* Append a message. Returns false when it doesn't fit (or the timestamp's
 * high bits differ from the packet's): send t->buf/t->len, call
 * sampan_blemidi_tx_reset and add it again. A message that can't fit in an
 * empty packet is dropped and true is returned. */
bool sampan_blemidi_add(struct sampan_blemidi_tx *t, const struct sampan_midi_msg *m,
                        uint16_t ts_ms);
void sampan_blemidi_tx_reset(struct sampan_blemidi_tx *t);

/* Strip the header and timestamps from a received packet and return the raw
 * MIDI bytes (feed them to a parser, which also follows SysEx across packets).
 * Timestamps are dropped. Returns the byte count, or -1 if the packet is
 * malformed or out is too small. */
int sampan_blemidi_decode(const uint8_t *pkt, size_t len, uint8_t *out, size_t cap);

/* Button gestures -> CC. 0xFF disables a slot. */
#define SAMPAN_CC_OFF 0xFF

struct sampan_btnmap {
    uint8_t channel; /* 0..15 */
    uint8_t cc_hold;   /* PRESS 127, RELEASE 0 */
    uint8_t cc_click;  /* CLICK, DOUBLE, LONG: 127 then 0 */
    uint8_t cc_double;
    uint8_t cc_long;
};

/* Defaults for button i (0..3): hold CC 80+i, click 102+i, double 106+i,
 * long 110+i, channel 1. */
void sampan_btnmap_default(struct sampan_btnmap *m, uint8_t i);
/* Messages for one button event (SAMPAN_BTN_*). Returns the count (0..2). */
int sampan_btnmap_event(const struct sampan_btnmap *m, uint8_t event,
                        struct sampan_midi_msg out[2]);

#ifdef __cplusplus
}
#endif

#endif
