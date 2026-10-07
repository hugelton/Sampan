/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Host-side tests: button gestures, MIDI parser, USB-MIDI, BLE MIDI, CC map.
 */
#include <string.h>

#include "check.h"
#include "sampan/button.h"
#include "sampan/midi.h"

/* ---- buttons ---------------------------------------------------------- */

struct ev_log {
    int n;
    uint8_t ev[64];
    uint32_t at[64];
};

/* Drive a button at 1 ms steps. `edges` lists the times (relative to t0)
 * where the raw level toggles, starting released. */
static void run_btn(struct sampan_btn *b, uint32_t t0, const uint32_t *edges, int n_edges,
                    uint32_t until, struct ev_log *log)
{
    bool level = false;
    int k = 0;
    log->n = 0;
    for (uint32_t t = 0; t <= until; t++) {
        while (k < n_edges && edges[k] == t) {
            level = !level;
            k++;
        }
        uint8_t ev[SAMPAN_BTN_MAX_EVENTS];
        int n = sampan_btn_update(b, level, t0 + t, ev);
        for (int i = 0; i < n && log->n < 64; i++) {
            log->ev[log->n] = ev[i];
            log->at[log->n++] = t;
        }
    }
}

static int count(const struct ev_log *l, uint8_t e)
{
    int c = 0;
    for (int i = 0; i < l->n; i++)
        c += l->ev[i] == e;
    return c;
}

static void test_button(void)
{
    struct sampan_btn b;
    struct ev_log log;

    /* Single click: PRESS after debounce, CLICK when the window closes. */
    sampan_btn_init(&b, NULL);
    const uint32_t click[] = {100, 200};
    run_btn(&b, 0, click, 2, 1000, &log);
    CHECK(log.n == 3);
    CHECK(log.ev[0] == SAMPAN_BTN_PRESS && log.at[0] == 110);
    CHECK(log.ev[1] == SAMPAN_BTN_RELEASE && log.at[1] == 210);
    CHECK(log.ev[2] == SAMPAN_BTN_CLICK && log.at[2] == 460);

    /* Contact bounce on both edges gives one press and one release. */
    sampan_btn_init(&b, NULL);
    const uint32_t bounce[] = {100, 102, 104, 106, 108, 300, 302, 304};
    run_btn(&b, 0, bounce, 8, 1000, &log);
    CHECK(count(&log, SAMPAN_BTN_PRESS) == 1);
    CHECK(count(&log, SAMPAN_BTN_RELEASE) == 1);
    CHECK(count(&log, SAMPAN_BTN_CLICK) == 1);

    /* Double click. */
    sampan_btn_init(&b, NULL);
    const uint32_t dbl[] = {100, 180, 300, 380};
    run_btn(&b, 0, dbl, 4, 1500, &log);
    CHECK(count(&log, SAMPAN_BTN_PRESS) == 2);
    CHECK(count(&log, SAMPAN_BTN_DOUBLE) == 1);
    CHECK(count(&log, SAMPAN_BTN_CLICK) == 0);
    CHECK(log.ev[log.n - 1] == SAMPAN_BTN_DOUBLE && log.at[log.n - 1] == 390);

    /* Second press just inside the window but still debouncing at its end. */
    sampan_btn_init(&b, NULL);
    const uint32_t late[] = {100, 180, 435, 500};
    run_btn(&b, 0, late, 4, 1500, &log);
    CHECK(count(&log, SAMPAN_BTN_DOUBLE) == 1);
    CHECK(count(&log, SAMPAN_BTN_CLICK) == 0);

    /* Two slow clicks are two clicks. */
    sampan_btn_init(&b, NULL);
    const uint32_t slow[] = {100, 180, 700, 780};
    run_btn(&b, 0, slow, 4, 1500, &log);
    CHECK(count(&log, SAMPAN_BTN_CLICK) == 2);
    CHECK(count(&log, SAMPAN_BTN_DOUBLE) == 0);

    /* Long press fires while held; nothing else on release. */
    sampan_btn_init(&b, NULL);
    const uint32_t hold[] = {100, 1200};
    run_btn(&b, 0, hold, 2, 2000, &log);
    CHECK(log.n == 3);
    CHECK(log.ev[1] == SAMPAN_BTN_LONG && log.at[1] == 710);
    CHECK(log.ev[2] == SAMPAN_BTN_RELEASE);

    /* Click then a long hold: CLICK for the first, then LONG. */
    sampan_btn_init(&b, NULL);
    const uint32_t click_hold[] = {100, 180, 300, 1500};
    run_btn(&b, 0, click_hold, 4, 2000, &log);
    CHECK(count(&log, SAMPAN_BTN_CLICK) == 1);
    CHECK(count(&log, SAMPAN_BTN_LONG) == 1);
    CHECK(count(&log, SAMPAN_BTN_DOUBLE) == 0);

    /* Double click disabled: CLICK on release, no wait. */
    struct sampan_btn_cfg fast = {.debounce_ms = 5, .double_ms = 0, .long_ms = 500};
    sampan_btn_init(&b, &fast);
    const uint32_t two[] = {100, 150, 200, 250};
    run_btn(&b, 0, two, 4, 1000, &log);
    CHECK(count(&log, SAMPAN_BTN_CLICK) == 2);
    CHECK(count(&log, SAMPAN_BTN_DOUBLE) == 0);
    CHECK(log.ev[2] == SAMPAN_BTN_CLICK && log.at[2] == 155);

    /* The millisecond clock wrapping changes nothing. */
    sampan_btn_init(&b, NULL);
    run_btn(&b, 0xFFFFFF00u, click, 2, 1000, &log);
    CHECK(log.n == 3 && log.at[0] == 110 && log.at[2] == 460);
    sampan_btn_init(&b, NULL);
    run_btn(&b, 0xFFFFFE00u, hold, 2, 2000, &log);
    CHECK(count(&log, SAMPAN_BTN_LONG) == 1 && log.at[1] == 710);
}

/* ---- MIDI parser ------------------------------------------------------ */

#define MAXM 256

struct msgs {
    int n;
    struct sampan_midi_msg m[MAXM];
};

static void parse_bytes(struct sampan_midi_parser *p, const uint8_t *b, size_t len, struct msgs *out)
{
    for (size_t i = 0; i < len; i++) {
        struct sampan_midi_msg m[2];
        int n = sampan_midi_parse(p, b[i], m);
        for (int k = 0; k < n && out->n < MAXM; k++)
            out->m[out->n++] = m[k];
    }
}

static struct msgs parse(const uint8_t *b, size_t len)
{
    struct sampan_midi_parser p;
    static struct msgs out;
    sampan_midi_parser_init(&p);
    out.n = 0;
    parse_bytes(&p, b, len, &out);
    return out;
}

static int is(const struct sampan_midi_msg *m, uint8_t len, uint8_t a, uint8_t b, uint8_t c)
{
    return m->len == len && m->b[0] == a && (len < 2 || m->b[1] == b) && (len < 3 || m->b[2] == c);
}

static void test_parser(void)
{
    const uint8_t running[] = {0x90, 0x3C, 0x64, 0x3E, 0x64};
    struct msgs r = parse(running, sizeof(running));
    CHECK(r.n == 2 && is(&r.m[0], 3, 0x90, 0x3C, 0x64) && is(&r.m[1], 3, 0x90, 0x3E, 0x64));

    const uint8_t rt[] = {0x90, 0x3C, 0xF8, 0x64, 0xFD};
    r = parse(rt, sizeof(rt));
    CHECK(r.n == 2 && is(&r.m[0], 1, 0xF8, 0, 0) && is(&r.m[1], 3, 0x90, 0x3C, 0x64));

    const uint8_t pc[] = {0xC5, 0x10, 0x11};
    r = parse(pc, sizeof(pc));
    CHECK(r.n == 2 && is(&r.m[1], 2, 0xC5, 0x11, 0));

    const uint8_t sx[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    r = parse(sx, sizeof(sx));
    CHECK(r.n == 2);
    CHECK(r.m[0].sysex && !r.m[0].sysex_end && is(&r.m[0], 3, 0xF0, 0x7E, 0x7F));
    CHECK(r.m[1].sysex && r.m[1].sysex_end && is(&r.m[1], 3, 0x06, 0x01, 0xF7));

    const uint8_t sx3[] = {0xF0, 0x01, 0x02, 0xF7};
    r = parse(sx3, sizeof(sx3));
    CHECK(r.n == 2 && r.m[1].sysex_end && is(&r.m[1], 1, 0xF7, 0, 0));

    /* Realtime inside SysEx passes through; SysEx carries on. */
    const uint8_t sxrt[] = {0xF0, 0x01, 0xF8, 0x02, 0xF7};
    r = parse(sxrt, sizeof(sxrt));
    CHECK(r.n == 3 && is(&r.m[0], 1, 0xF8, 0, 0) && is(&r.m[1], 3, 0xF0, 0x01, 0x02));

    /* A status byte cuts SysEx short; it is closed with F7. */
    const uint8_t cut[] = {0xF0, 0x01, 0x90, 0x3C, 0x64};
    r = parse(cut, sizeof(cut));
    CHECK(r.n == 2 && r.m[0].sysex_end && is(&r.m[0], 3, 0xF0, 0x01, 0xF7));
    CHECK(is(&r.m[1], 3, 0x90, 0x3C, 0x64));

    /* System common: no running status; F4 clears it; stray data ignored. */
    const uint8_t sys[] = {0x3C, 0xF3, 0x05, 0x06, 0xF6, 0xF2, 0x01, 0x02,
                           0x90, 0x3C, 0x64, 0xF4, 0x3E, 0x64, 0xF7};
    r = parse(sys, sizeof(sys));
    CHECK(r.n == 4);
    CHECK(is(&r.m[0], 2, 0xF3, 0x05, 0) && is(&r.m[1], 1, 0xF6, 0, 0));
    CHECK(is(&r.m[2], 3, 0xF2, 0x01, 0x02) && is(&r.m[3], 3, 0x90, 0x3C, 0x64));
}

/* ---- USB-MIDI ----------------------------------------------------------- */

static void test_usbmidi(void)
{
    struct sampan_midi_msg m = {.len = 3, .b = {0x93, 0x40, 0x7F}};
    uint8_t pkt[4], raw[3];
    CHECK(sampan_usbmidi_pack(2, &m, pkt));
    CHECK(pkt[0] == 0x29 && pkt[1] == 0x93 && pkt[2] == 0x40 && pkt[3] == 0x7F);
    CHECK(sampan_usbmidi_unpack(pkt, raw) == 3 && memcmp(raw, m.b, 3) == 0);

    struct {
        struct sampan_midi_msg m;
        uint8_t cin;
    } cases[] = {
        {{.len = 1, .b = {0xF8}}, 0xF},
        {{.len = 1, .b = {0xF6}}, 0x5},
        {{.len = 2, .b = {0xF1, 0x10}}, 0x2},
        {{.len = 3, .b = {0xF2, 0x01, 0x02}}, 0x3},
        {{.len = 2, .b = {0xD0, 0x40}}, 0xD},
        {{.len = 3, .b = {0xF0, 1, 2}, .sysex = true}, 0x4},
        {{.len = 1, .b = {0xF7}, .sysex = true, .sysex_end = true}, 0x5},
        {{.len = 2, .b = {3, 0xF7}, .sysex = true, .sysex_end = true}, 0x6},
        {{.len = 3, .b = {3, 4, 0xF7}, .sysex = true, .sysex_end = true}, 0x7},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(sampan_usbmidi_pack(0, &cases[i].m, pkt));
        CHECK((pkt[0] & 0x0F) == cases[i].cin);
        CHECK(sampan_usbmidi_unpack(pkt, raw) == cases[i].m.len);
        CHECK(memcmp(raw, cases[i].m.b, cases[i].m.len) == 0);
    }

    struct sampan_midi_msg bad = {.len = 2, .b = {0xF0, 1}, .sysex = true};
    CHECK(!sampan_usbmidi_pack(0, &bad, pkt));
    struct sampan_midi_msg empty = {0};
    CHECK(!sampan_usbmidi_pack(0, &empty, pkt));
}

/* ---- round trips through USB and BLE ----------------------------------- */

/* A stream with notes, running status, realtime, SysEx of awkward lengths. */
static size_t make_stream(uint8_t *s)
{
    size_t n = 0;
    const uint8_t a[] = {0x90, 0x3C, 0x64, 0x3E, 0x64, 0xF8, 0x40, 0x64, 0xB1, 0x07, 0x7F,
                         0xE0, 0x00, 0x40, 0xC2, 0x05, 0xF6, 0xFA};
    memcpy(s + n, a, sizeof(a));
    n += sizeof(a);
    for (int len = 0; len < 40; len += 7) {
        s[n++] = 0xF0;
        for (int i = 0; i < len; i++) {
            s[n++] = (uint8_t)(i * 13 & 0x7F);
            if (i == 5)
                s[n++] = 0xF8;
        }
        s[n++] = 0xF7;
        s[n++] = 0x80;
        s[n++] = (uint8_t)len;
        s[n++] = 0x00;
    }
    return n;
}

static int same(const struct msgs *a, const struct msgs *b)
{
    if (a->n != b->n)
        return 0;
    for (int i = 0; i < a->n; i++) {
        const struct sampan_midi_msg *x = &a->m[i], *y = &b->m[i];
        if (x->len != y->len || memcmp(x->b, y->b, x->len) || x->sysex != y->sysex ||
            x->sysex_end != y->sysex_end)
            return 0;
    }
    return 1;
}

static void test_roundtrip(void)
{
    static uint8_t stream[1024];
    size_t len = make_stream(stream);
    struct msgs ref = parse(stream, len);
    CHECK(ref.n > 20);

    /* USB: messages -> packets -> bytes -> parser */
    struct sampan_midi_parser p;
    static struct msgs got;
    sampan_midi_parser_init(&p);
    got.n = 0;
    for (int i = 0; i < ref.n; i++) {
        uint8_t pkt[4], raw[3];
        CHECK(sampan_usbmidi_pack(0, &ref.m[i], pkt));
        int n = sampan_usbmidi_unpack(pkt, raw);
        parse_bytes(&p, raw, (size_t)n, &got);
    }
    CHECK(same(&ref, &got));

    /* BLE at a small and a large MTU, timestamps crossing the 7-bit and
     * 13-bit wraps. */
    const size_t mtus[] = {8, 20, 64, 247};
    for (size_t k = 0; k < sizeof(mtus) / sizeof(mtus[0]); k++) {
        struct sampan_blemidi_tx tx;
        sampan_blemidi_tx_init(&tx, mtus[k]);
        sampan_midi_parser_init(&p);
        got.n = 0;
        int packets = 0, bad = 0;
        uint16_t ts = 8100;
        for (int i = 0; i <= ref.n; i++) {
            bool flush = i == ref.n;
            if (!flush && !sampan_blemidi_add(&tx, &ref.m[i], ts))
                flush = true;
            if (flush && tx.len) {
                uint8_t raw[SAMPAN_BLEMIDI_MAX];
                CHECK(tx.len <= mtus[k]);
                int n = sampan_blemidi_decode(tx.buf, tx.len, raw, sizeof(raw));
                bad += n < 0;
                if (n > 0)
                    parse_bytes(&p, raw, (size_t)n, &got);
                sampan_blemidi_tx_reset(&tx);
                packets++;
                if (i < ref.n)
                    i--; /* add it again */
            }
            ts = (uint16_t)((ts + 37) & 0x1FFF);
        }
        CHECK(bad == 0);
        CHECK(same(&ref, &got));
        CHECK(packets >= 1);
    }
}

static void test_blemidi(void)
{
    /* Two notes, the second with running status. */
    const uint8_t pkt[] = {0x80, 0x80, 0x90, 0x3C, 0x64, 0x81, 0x3E, 0x64};
    uint8_t raw[16];
    int n = sampan_blemidi_decode(pkt, sizeof(pkt), raw, sizeof(raw));
    const uint8_t want[] = {0x90, 0x3C, 0x64, 0x3E, 0x64};
    CHECK(n == 5 && memcmp(raw, want, 5) == 0);

    /* SysEx continuing in a new packet: data straight after the header. */
    const uint8_t cont[] = {0x81, 0x11, 0x22, 0x85, 0xF7};
    n = sampan_blemidi_decode(cont, sizeof(cont), raw, sizeof(raw));
    CHECK(n == 3 && raw[0] == 0x11 && raw[2] == 0xF7);

    const uint8_t bad1[] = {0x40, 0x80, 0x90};
    const uint8_t bad2[] = {0x80, 0x80, 0x90, 0x3C, 0x64, 0x81};
    CHECK(sampan_blemidi_decode(bad1, sizeof(bad1), raw, sizeof(raw)) < 0);
    CHECK(sampan_blemidi_decode(bad2, sizeof(bad2), raw, sizeof(raw)) < 0);
    CHECK(sampan_blemidi_decode(pkt, sizeof(pkt), raw, 2) < 0);

    /* Header carries the high bits; a different high part needs a new packet. */
    struct sampan_blemidi_tx tx;
    sampan_blemidi_tx_init(&tx, 20);
    struct sampan_midi_msg on = {.len = 3, .b = {0x90, 0x3C, 0x64}};
    CHECK(sampan_blemidi_add(&tx, &on, 0x1FFF));
    CHECK(tx.buf[0] == 0xBF && tx.buf[1] == 0xFF && tx.len == 5);
    CHECK(!sampan_blemidi_add(&tx, &on, 0x0000));
    CHECK(sampan_blemidi_add(&tx, &on, 0x1F80));
    /* Fill to the limit. */
    while (sampan_blemidi_add(&tx, &on, 0x1F80))
        ;
    CHECK(tx.len <= 20 && tx.len + 4 > 20);
}

static void test_btnmap(void)
{
    struct sampan_btnmap m;
    struct sampan_midi_msg out[2];
    sampan_btnmap_default(&m, 1);
    CHECK(m.cc_hold == 81 && m.cc_click == 103 && m.cc_double == 107 && m.cc_long == 111);

    CHECK(sampan_btnmap_event(&m, SAMPAN_BTN_PRESS, out) == 1);
    CHECK(is(&out[0], 3, 0xB0, 81, 127));
    CHECK(sampan_btnmap_event(&m, SAMPAN_BTN_RELEASE, out) == 1);
    CHECK(is(&out[0], 3, 0xB0, 81, 0));
    CHECK(sampan_btnmap_event(&m, SAMPAN_BTN_DOUBLE, out) == 2);
    CHECK(is(&out[0], 3, 0xB0, 107, 127) && is(&out[1], 3, 0xB0, 107, 0));

    m.channel = 15;
    m.cc_long = SAMPAN_CC_OFF;
    CHECK(sampan_btnmap_event(&m, SAMPAN_BTN_LONG, out) == 0);
    CHECK(sampan_btnmap_event(&m, SAMPAN_BTN_CLICK, out) == 2 && out[0].b[0] == 0xBF);
    CHECK(sampan_btnmap_event(&m, 99, out) == 0);
}

int main(void)
{
    test_button();
    test_parser();
    test_usbmidi();
    test_roundtrip();
    test_blemidi();
    test_btnmap();
    return CHECK_DONE();
}
