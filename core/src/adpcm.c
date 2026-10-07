/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/adpcm.h"

static const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

void sampan_adpcm_reset(struct sampan_adpcm *s)
{
    s->pred = 0;
    s->index = 0;
}

/* Shared by both sides so the encoder tracks the decoder exactly. */
static void step(struct sampan_adpcm *s, uint8_t nib)
{
    int st = step_table[s->index];
    int diff = st >> 3;
    if (nib & 4)
        diff += st;
    if (nib & 2)
        diff += st >> 1;
    if (nib & 1)
        diff += st >> 2;
    int pred = s->pred + ((nib & 8) ? -diff : diff);
    if (pred > 32767)
        pred = 32767;
    else if (pred < -32768)
        pred = -32768;
    s->pred = (int16_t)pred;

    int idx = s->index + index_table[nib];
    if (idx < 0)
        idx = 0;
    else if (idx > 88)
        idx = 88;
    s->index = (uint8_t)idx;
}

static uint8_t encode1(struct sampan_adpcm *s, int16_t sample)
{
    int st = step_table[s->index];
    int diff = sample - s->pred;
    uint8_t nib = 0;
    if (diff < 0) {
        nib = 8;
        diff = -diff;
    }
    if (diff >= st) {
        nib |= 4;
        diff -= st;
    }
    if (diff >= (st >> 1)) {
        nib |= 2;
        diff -= st >> 1;
    }
    if (diff >= (st >> 2))
        nib |= 1;
    step(s, nib);
    return nib;
}

void sampan_adpcm_encode(struct sampan_adpcm *s, const int16_t *in, size_t stride,
                         size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t nib = encode1(s, in[i * stride]);
        if (i & 1)
            out[i >> 1] |= (uint8_t)(nib << 4);
        else
            out[i >> 1] = nib;
    }
}

void sampan_adpcm_decode(struct sampan_adpcm *s, const uint8_t *in, size_t n,
                         int16_t *out, size_t stride)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t nib = (i & 1) ? (uint8_t)(in[i >> 1] >> 4) : (uint8_t)(in[i >> 1] & 0x0F);
        step(s, nib);
        out[i * stride] = s->pred;
    }
}
