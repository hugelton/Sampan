/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/midi.h"

#include <string.h>

#include "sampan/button.h"

uint8_t sampan_midi_msg_len(uint8_t s)
{
    if (s < 0x80)
        return 0;
    if (s < 0xC0 || (s >= 0xE0 && s < 0xF0))
        return 3;
    if (s < 0xE0)
        return 2;
    switch (s) {
    case 0xF1: case 0xF3: return 2;
    case 0xF2: return 3;
    case 0xF6: case 0xF8: case 0xFA: case 0xFB: case 0xFC: case 0xFE: case 0xFF: return 1;
    default: return 0; /* F0, F4, F5, F7, F9, FD */
    }
}

void sampan_midi_parser_init(struct sampan_midi_parser *p)
{
    memset(p, 0, sizeof(*p));
}

static void emit(struct sampan_midi_msg *m, const uint8_t *b, uint8_t len, bool sysex, bool end)
{
    memset(m, 0, sizeof(*m));
    memcpy(m->b, b, len);
    m->len = len;
    m->sysex = sysex;
    m->sysex_end = end;
}

int sampan_midi_parse(struct sampan_midi_parser *p, uint8_t x, struct sampan_midi_msg out[2])
{
    int n = 0;

    if (x >= 0xF8) { /* realtime: anywhere, changes nothing */
        if (sampan_midi_msg_len(x))
            emit(&out[n++], &x, 1, false, false);
        return n;
    }

    if (p->in_sysex) {
        if (x < 0x80) {
            p->sx[p->sx_len++] = x;
            if (p->sx_len == 3) {
                emit(&out[n++], p->sx, 3, true, false);
                p->sx_len = 0;
            }
            return n;
        }
        /* F7 ends it; any other status cuts it short and gets an F7 here. */
        p->sx[p->sx_len++] = 0xF7;
        emit(&out[n++], p->sx, p->sx_len, true, true);
        p->sx_len = 0;
        p->in_sysex = false;
        if (x == 0xF7)
            return n;
    }

    if (x == 0xF0) {
        p->in_sysex = true;
        p->sx[0] = 0xF0;
        p->sx_len = 1;
        p->status = 0;
        return n;
    }

    if (x >= 0x80) {
        uint8_t len = sampan_midi_msg_len(x);
        p->status = 0;
        p->have = 0;
        if (len == 1)
            emit(&out[n++], &x, 1, false, false);
        else if (len > 1) {
            p->status = x;
            p->need = (uint8_t)(len - 1);
        }
        return n;
    }

    if (p->status == 0)
        return n; /* data with no status */
    p->data[p->have++] = x;
    if (p->have == p->need) {
        uint8_t b[3] = {p->status, p->data[0], p->data[1]};
        emit(&out[n++], b, (uint8_t)(p->need + 1), false, false);
        p->have = 0;
        if (p->status >= 0xF0)
            p->status = 0; /* no running status for system common */
    }
    return n;
}

bool sampan_usbmidi_pack(uint8_t cable, const struct sampan_midi_msg *m, uint8_t pkt[4])
{
    uint8_t cin;
    if (m->len == 0 || m->len > 3)
        return false;
    if (m->sysex) {
        if (!m->sysex_end && m->len != 3)
            return false;
        cin = m->sysex_end ? (uint8_t)(4 + m->len) : 4;
    } else {
        uint8_t s = m->b[0];
        if (s < 0x80)
            return false;
        if (s < 0xF0)
            cin = s >> 4;
        else if (s >= 0xF8)
            cin = 0xF;
        else
            cin = m->len == 1 ? 5 : m->len == 2 ? 2 : 3;
    }
    pkt[0] = (uint8_t)(cable << 4 | cin);
    pkt[1] = pkt[2] = pkt[3] = 0;
    memcpy(pkt + 1, m->b, m->len);
    return true;
}

int sampan_usbmidi_unpack(const uint8_t pkt[4], uint8_t out[3])
{
    static const uint8_t len_by_cin[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    int n = len_by_cin[pkt[0] & 0x0F];
    memcpy(out, pkt + 1, (size_t)n);
    return n;
}

void sampan_blemidi_tx_init(struct sampan_blemidi_tx *t, size_t max_payload)
{
    memset(t, 0, sizeof(*t));
    if (max_payload < 8)
        max_payload = 8;
    if (max_payload > SAMPAN_BLEMIDI_MAX)
        max_payload = SAMPAN_BLEMIDI_MAX;
    t->max = max_payload;
}

void sampan_blemidi_tx_reset(struct sampan_blemidi_tx *t)
{
    t->len = 0;
}

bool sampan_blemidi_add(struct sampan_blemidi_tx *t, const struct sampan_midi_msg *m,
                        uint16_t ts_ms)
{
    uint8_t hi = (uint8_t)((ts_ms >> 7) & 0x3F), lo = (uint8_t)(ts_ms & 0x7F);

    /* Every status byte (including F0 and F7) gets a timestamp before it;
     * SysEx data bytes don't. */
    size_t need = 0;
    for (uint8_t i = 0; i < m->len; i++)
        need += (m->b[i] & 0x80) ? 2 : 1;
    if (m->len == 0)
        return true;

    if (t->len == 0) {
        if (1 + need > t->max)
            return true; /* can never fit: drop */
        t->buf[0] = (uint8_t)(0x80 | hi);
        t->ts_high = hi;
        t->len = 1;
    } else if (hi != t->ts_high || t->len + need > t->max) {
        return false;
    }

    for (uint8_t i = 0; i < m->len; i++) {
        if (m->b[i] & 0x80)
            t->buf[t->len++] = (uint8_t)(0x80 | lo);
        t->buf[t->len++] = m->b[i];
    }
    return true;
}

int sampan_blemidi_decode(const uint8_t *pkt, size_t len, uint8_t *out, size_t cap)
{
    if (len < 1 || (pkt[0] & 0xC0) != 0x80)
        return -1;
    size_t o = 0;
    bool after_ts = false;
    for (size_t i = 1; i < len; i++) {
        uint8_t b = pkt[i];
        if ((b & 0x80) && !after_ts) {
            after_ts = true; /* timestamp low byte */
            continue;
        }
        after_ts = false;
        if (o >= cap)
            return -1;
        out[o++] = b;
    }
    return after_ts ? -1 : (int)o;
}

void sampan_btnmap_default(struct sampan_btnmap *m, uint8_t i)
{
    i &= 3;
    m->channel = 0;
    m->cc_hold = (uint8_t)(80 + i);
    m->cc_click = (uint8_t)(102 + i);
    m->cc_double = (uint8_t)(106 + i);
    m->cc_long = (uint8_t)(110 + i);
}

static void cc(struct sampan_midi_msg *m, uint8_t ch, uint8_t num, uint8_t val)
{
    uint8_t b[3] = {(uint8_t)(0xB0 | (ch & 0x0F)), num, val};
    emit(m, b, 3, false, false);
}

int sampan_btnmap_event(const struct sampan_btnmap *m, uint8_t event,
                        struct sampan_midi_msg out[2])
{
    uint8_t num;
    switch (event) {
    case SAMPAN_BTN_PRESS:
    case SAMPAN_BTN_RELEASE:
        if (m->cc_hold > 127)
            return 0;
        cc(&out[0], m->channel, m->cc_hold, event == SAMPAN_BTN_PRESS ? 127 : 0);
        return 1;
    case SAMPAN_BTN_CLICK: num = m->cc_click; break;
    case SAMPAN_BTN_DOUBLE: num = m->cc_double; break;
    case SAMPAN_BTN_LONG: num = m->cc_long; break;
    default: return 0;
    }
    if (num > 127)
        return 0;
    cc(&out[0], m->channel, num, 127);
    cc(&out[1], m->channel, num, 0);
    return 2;
}
