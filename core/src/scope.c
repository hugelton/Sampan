/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/scope.h"

#include <string.h>

#define FRESH 0x4u
#define INDEX 0x3u

void sampan_scope_cfg_default(struct sampan_scope_cfg *c, uint16_t width, uint32_t rate)
{
    c->width = width;
    c->window = rate / 50; /* 20 ms */
    c->trigger = true;
    c->trig_level = 0;
    c->trig_hyst = 200;
    c->auto_samples = rate / 10;
}

/* Column x shows samples [x * window / width, (x + 1) * window / width), and
 * at least the first of those, so when zoomed in past one sample per column a
 * sample fills several columns. */
static uint32_t col_start(const struct sampan_scope *s, uint32_t x)
{
    return (uint32_t)((uint64_t)x * s->cfg.window / s->cfg.width);
}

/* Can equal the start when zoomed in: the column then just takes the sample
 * at its start and the next column begins on the same sample. */
static uint32_t col_end(const struct sampan_scope *s, uint32_t x)
{
    return col_start(s, x + 1);
}

static void enter_col(struct sampan_scope *s, struct sampan_scope_frame *f, uint16_t x)
{
    s->col = x;
    s->col_start = col_start(s, x);
    s->col_end = col_end(s, x);
    f->lo[x] = INT16_MAX;
    f->hi[x] = INT16_MIN;
    f->clip[x] = 0;
}

static void start_frame(struct sampan_scope *s, bool triggered)
{
    struct sampan_scope_frame *f = &s->frames[s->back];
    f->width = s->cfg.width;
    f->triggered = triggered;
    s->capturing = true;
    s->pos = 0;
    enter_col(s, f, 0);
}

static void rearm(struct sampan_scope *s)
{
    s->capturing = false;
    s->armed_low = false;
    s->waited = 0;
}

void sampan_scope_init(struct sampan_scope *s, const struct sampan_scope_cfg *cfg)
{
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    if (s->cfg.width < 1)
        s->cfg.width = 1;
    if (s->cfg.width > SAMPAN_SCOPE_MAX_W)
        s->cfg.width = SAMPAN_SCOPE_MAX_W;
    if (s->cfg.window < 1)
        s->cfg.window = 1;
    s->back = 0;
    s->front = 1;
    atomic_init(&s->middle, 2);
    rearm(s);
}

static void finish_frame(struct sampan_scope *s)
{
    struct sampan_scope_frame *f = &s->frames[s->back];
    f->seq = ++s->seq;
    s->back = (uint8_t)(atomic_exchange_explicit(&s->middle, s->back | FRESH,
                                                 memory_order_acq_rel) & INDEX);
    rearm(s);
}

void sampan_scope_push(struct sampan_scope *s, const int16_t *x, size_t n, size_t stride)
{
    const struct sampan_scope_cfg *c = &s->cfg;
    for (size_t i = 0; i < n; i++) {
        int16_t v = x[i * stride];

        if (!s->capturing) {
            if (!c->trigger || s->waited >= c->auto_samples) {
                start_frame(s, false);
            } else {
                s->waited++;
                if ((int32_t)v <= (int32_t)c->trig_level - c->trig_hyst) {
                    s->armed_low = true;
                    continue;
                }
                if (!(s->armed_low && v >= c->trig_level))
                    continue;
                start_frame(s, true);
            }
        }

        struct sampan_scope_frame *f = &s->frames[s->back];
        while (s->col < c->width && s->col_start <= s->pos) {
            uint16_t col = s->col;
            if (v < f->lo[col])
                f->lo[col] = v;
            if (v > f->hi[col])
                f->hi[col] = v;
            if (v == INT16_MAX || v == INT16_MIN)
                f->clip[col] = 1;
            if (s->pos + 1 < s->col_end)
                break;
            if (col + 1 < c->width)
                enter_col(s, f, (uint16_t)(col + 1));
            else
                s->col = c->width;
        }
        s->pos++;
        if (s->col >= c->width)
            finish_frame(s);
    }
}

const struct sampan_scope_frame *sampan_scope_latest(struct sampan_scope *s, bool *fresh)
{
    bool got = false;
    if (atomic_load_explicit(&s->middle, memory_order_acquire) & FRESH) {
        s->front = (uint8_t)(atomic_exchange_explicit(&s->middle, s->front,
                                                      memory_order_acq_rel) & INDEX);
        got = true;
    }
    if (fresh)
        *fresh = got;
    const struct sampan_scope_frame *f = &s->frames[s->front];
    return f->seq ? f : NULL;
}

void sampan_scope_style_default(struct sampan_scope_style *st, uint8_t palette)
{
    const struct sampan_palette *p = &sampan_palettes[palette % SAMPAN_PAL_COUNT];
    st->bg = SAMPAN_BLACK;
    st->axis = p->c[0];
    st->wave = p->c[4];
    st->clip = SAMPAN_WHITE;
    st->auto_scale = true;
    st->auto_floor = 1500;
    st->zoom_q8 = 256;
    st->swap_bytes = false;
}

void sampan_scope_spans(const struct sampan_scope_frame *f, uint16_t height,
                        const struct sampan_scope_style *st, struct sampan_scope_spans *sp)
{
    uint16_t w = f->width;
    int32_t peak;
    if (st->auto_scale) {
        peak = st->auto_floor > 0 ? st->auto_floor : 1;
        for (uint16_t x = 0; x < w; x++) {
            if (-(int32_t)f->lo[x] > peak)
                peak = -(int32_t)f->lo[x];
            if (f->hi[x] > peak)
                peak = f->hi[x];
        }
    } else {
        peak = (int32_t)(32768L * 256 / (st->zoom_q8 ? st->zoom_q8 : 1));
        if (peak < 1)
            peak = 1;
    }

    /* Peak reaches 88% of the half height, as in the synth's scope. */
    int32_t mid = height / 2, amp = (int32_t)height * 44 / 100;
    sp->width = w;
    sp->height = height;
    sp->axis_y = (uint16_t)mid;

    int32_t prev0 = 0, prev1 = 0;
    for (uint16_t x = 0; x < w; x++) {
        int32_t top = mid - (int32_t)f->hi[x] * amp / peak;
        int32_t bot = mid - (int32_t)f->lo[x] * amp / peak;
        if (top < 0)
            top = 0;
        if (bot > height - 1)
            bot = height - 1;
        if (top > bot) /* only possible after clamping */
            top = bot;
        int32_t y0 = top, y1 = bot;
        if (x > 0) { /* reach the previous column's raw span */
            if (y0 > prev1 + 1)
                y0 = prev1 + 1;
            if (y1 < prev0 - 1)
                y1 = prev0 - 1;
        }
        prev0 = top;
        prev1 = bot;
        sp->y0[x] = (uint16_t)y0;
        sp->y1[x] = (uint16_t)y1;
        sp->clip[x] = f->clip[x];
    }
}

static uint16_t px(uint16_t c, bool swap)
{
    return swap ? (uint16_t)(c >> 8 | c << 8) : c;
}

void sampan_scope_render(const struct sampan_scope_spans *sp, const struct sampan_scope_style *st,
                         uint16_t y, uint16_t rows, uint16_t *out)
{
    uint16_t bg = px(st->bg, st->swap_bytes), axis = px(st->axis, st->swap_bytes);
    uint16_t wave = px(st->wave, st->swap_bytes), clip = px(st->clip, st->swap_bytes);
    for (uint16_t r = 0; r < rows; r++) {
        uint16_t yy = (uint16_t)(y + r);
        uint16_t base = yy == sp->axis_y ? axis : bg;
        uint16_t *row = out + (size_t)r * sp->width;
        for (uint16_t x = 0; x < sp->width; x++)
            row[x] = (yy >= sp->y0[x] && yy <= sp->y1[x]) ? (sp->clip[x] ? clip : wave) : base;
    }
}
