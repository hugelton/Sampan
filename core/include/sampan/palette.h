/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Screen palettes: five steps of one colour, darkest to brightest, on black.
 * White is kept for accents (what is being touched, warnings).
 *   0 rules and axes   1 inactive   2 labels   3 secondary text   4 values, traces
 */
#ifndef SAMPAN_PALETTE_H
#define SAMPAN_PALETTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAMPAN_RGB565(r, g, b) \
    ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define SAMPAN_BLACK 0x0000u
#define SAMPAN_WHITE 0xFFFFu

enum { SAMPAN_PAL_GREEN, SAMPAN_PAL_AMBER, SAMPAN_PAL_CYAN, SAMPAN_PAL_RED, SAMPAN_PAL_MONO,
       SAMPAN_PAL_COUNT };
#define SAMPAN_PAL_DEFAULT SAMPAN_PAL_MONO

struct sampan_palette {
    const char *name;
    uint16_t c[5];
};

extern const struct sampan_palette sampan_palettes[SAMPAN_PAL_COUNT];

#ifdef __cplusplus
}
#endif

#endif
