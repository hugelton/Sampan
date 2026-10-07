/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * IMA ADPCM, mono, 4 bits per sample. Samples are packed low nibble first;
 * an odd count leaves the last high nibble 0.
 */
#ifndef SAMPAN_ADPCM_H
#define SAMPAN_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sampan_adpcm {
    int16_t pred;
    uint8_t index; /* 0..88 */
};

void sampan_adpcm_reset(struct sampan_adpcm *s);

/* `stride` steps through interleaved input/output (1 for mono). The state is
 * carried across calls; out/in hold (n + 1) / 2 bytes. */
void sampan_adpcm_encode(struct sampan_adpcm *s, const int16_t *in, size_t stride,
                         size_t n, uint8_t *out);
void sampan_adpcm_decode(struct sampan_adpcm *s, const uint8_t *in, size_t n,
                         int16_t *out, size_t stride);

#ifdef __cplusplus
}
#endif

#endif
