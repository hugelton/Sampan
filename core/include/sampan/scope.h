/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Waveform display. The audio side pushes samples; each frame keeps only the
 * min and max per screen column, starting at a rising edge (a Schmitt trigger
 * with hysteresis) so periodic sounds stand still, or free-running when no
 * edge comes. Frames pass to the display side through a lock-free triple
 * buffer: one writer (audio) and one reader (display), on any cores.
 *
 * Rendering is RGB565 in horizontal bands, so a display driver can draw a
 * few rows at a time without a full frame buffer.
 */
#ifndef SAMPAN_SCOPE_H
#define SAMPAN_SCOPE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sampan/palette.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAMPAN_SCOPE_MAX_W 320

struct sampan_scope_cfg {
    uint16_t width;        /* columns, 1..SAMPAN_SCOPE_MAX_W */
    uint32_t window;       /* samples across the screen (>= 1) */
    bool trigger;          /* false: free-run */
    int16_t trig_level;    /* rising edge through this level ... */
    int16_t trig_hyst;     /* ... after being at or below level - hyst */
    uint32_t auto_samples; /* free-run a frame after this long without an edge */
};

/* Sensible defaults: 20 ms across the screen, trigger at 0 with a little
 * hysteresis, auto after 100 ms. */
void sampan_scope_cfg_default(struct sampan_scope_cfg *c, uint16_t width, uint32_t rate);

struct sampan_scope_frame {
    uint16_t width;
    bool triggered; /* false when the frame was free-run */
    uint32_t seq;
    int16_t lo[SAMPAN_SCOPE_MAX_W];
    int16_t hi[SAMPAN_SCOPE_MAX_W];
    uint8_t clip[SAMPAN_SCOPE_MAX_W]; /* a sample hit full scale */
};

struct sampan_scope {
    struct sampan_scope_cfg cfg;
    struct sampan_scope_frame frames[3];
    /* triple buffer */
    uint8_t back, front;
    atomic_uint_fast8_t middle; /* index | SAMPAN_SCOPE_FRESH */
    /* writer state */
    bool capturing, armed_low;
    uint32_t waited, pos;
    uint16_t col;
    uint32_t col_start, col_end; /* sample range of the current column */
    uint32_t seq;
};

void sampan_scope_init(struct sampan_scope *s, const struct sampan_scope_cfg *cfg);

/* Writer: feed samples; `stride` picks every n-th (1 for mono). */
void sampan_scope_push(struct sampan_scope *s, const int16_t *x, size_t n, size_t stride);

/* Reader: the newest finished frame (NULL before the first). *fresh says
 * whether it is new since the last call. */
const struct sampan_scope_frame *sampan_scope_latest(struct sampan_scope *s, bool *fresh);

/* ---- rendering ---------------------------------------------------------- */

/* Same look as the synth UI: black, one colour in five steps, white only as
 * an accent. The axis is the darkest step, the trace the brightest, and a
 * column that hit full scale is drawn in white. No grid. */
struct sampan_scope_style {
    uint16_t bg, axis, wave, clip; /* RGB565 */
    bool auto_scale;               /* scale each frame to its own peak ... */
    int16_t auto_floor;            /* ... but never zoom in past this peak */
    uint16_t zoom_q8;              /* fixed gain when !auto_scale, 256 = full scale */
    bool swap_bytes;               /* byte-swap pixels for big-endian SPI panels */
};

void sampan_scope_style_default(struct sampan_scope_style *st, uint8_t palette);

/* Per-column vertical spans for one frame, joined to their neighbours so the
 * trace has no gaps. */
struct sampan_scope_spans {
    uint16_t width, height;
    uint16_t axis_y;
    uint16_t y0[SAMPAN_SCOPE_MAX_W], y1[SAMPAN_SCOPE_MAX_W]; /* inclusive, y0 <= y1 */
    uint8_t clip[SAMPAN_SCOPE_MAX_W];
};

void sampan_scope_spans(const struct sampan_scope_frame *f, uint16_t height,
                        const struct sampan_scope_style *st, struct sampan_scope_spans *sp);

/* Draw rows [y, y + rows) into out (rows * width pixels, row-major). */
void sampan_scope_render(const struct sampan_scope_spans *sp, const struct sampan_scope_style *st,
                         uint16_t y, uint16_t rows, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif
