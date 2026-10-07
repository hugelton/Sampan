/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * A made-up IMA ADPCM stream that walks the step index up and down through
 * every entry of the tables, with every nibble value and with clipping, laid
 * out as 34-byte IMA4 packets (big-endian header: predictor top 9 bits and
 * step index, then 64 samples, low nibble first).
 *
 * The expected decode of this stream comes from an independent decoder
 * (macOS afconvert); see test_adpcm_sweep. A decoder following the IMA4
 * convention keeps its own state across a packet header that agrees with it
 * (same index, predictor within 127), which is what ima4_sweep_decode does.
 */
#ifndef SAMPAN_TESTS_IMA4_SWEEP_H
#define SAMPAN_TESTS_IMA4_SWEEP_H

#include <stdint.h>

#include "sampan/adpcm.h"

#define IMA4_SWEEP_PACKETS 40

static void ima4_sweep_make(uint8_t pk[IMA4_SWEEP_PACKETS][34])
{
    uint32_t r = 2463534242u;
    struct sampan_adpcm st = {0, 0};
    int t = 0;
    for (int k = 0; k < IMA4_SWEEP_PACKETS; k++) {
        uint16_t h = (uint16_t)((uint16_t)st.pred & 0xFF80u) | st.index;
        pk[k][0] = (uint8_t)(h >> 8);
        pk[k][1] = (uint8_t)h;
        for (int i = 0; i < 64; i++, t++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            int phase = t % 400; /* up quickly, then down one step at a time */
            int target = phase < 200 ? phase * 88 / 199 : (399 - phase) * 88 / 199;
            uint8_t nib = (uint8_t)((st.index < target ? 4 + r % 4 : r % 4) | (r >> 8 & 8));
            uint8_t byte = nib;
            int16_t out;
            sampan_adpcm_decode(&st, &byte, 1, &out, 1);
            if (i & 1)
                pk[k][2 + i / 2] |= (uint8_t)(nib << 4);
            else
                pk[k][2 + i / 2] = nib;
        }
    }
}

static void ima4_sweep_decode(uint8_t pk[IMA4_SWEEP_PACKETS][34],
                              int16_t out[IMA4_SWEEP_PACKETS * 64])
{
    struct sampan_adpcm st = {0, 0};
    for (int k = 0; k < IMA4_SWEEP_PACKETS; k++) {
        uint16_t h = (uint16_t)(pk[k][0] << 8 | pk[k][1]);
        int hp = (int16_t)(h & 0xFF80u), hi = h & 0x7F, d = hp - st.pred;
        if (hi != st.index || d > 0x7F || d < -0x7F) {
            st.pred = (int16_t)hp;
            st.index = (uint8_t)hi;
        }
        sampan_adpcm_decode(&st, pk[k] + 2, 64, out + k * 64, 1);
    }
}

static uint64_t ima4_sweep_hash(const int16_t *s, int n)
{
    uint64_t h = 0xcbf29ce484222325u; /* FNV-1a over little-endian samples */
    for (int i = 0; i < n; i++) {
        h = (h ^ (uint8_t)s[i]) * 0x100000001b3u;
        h = (h ^ (uint8_t)((uint16_t)s[i] >> 8)) * 0x100000001b3u;
    }
    return h;
}

#endif
