/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Sender side: PCM in, wire-ready frames out.
 */
#ifndef SAMPAN_TX_H
#define SAMPAN_TX_H

#include <stddef.h>
#include <stdint.h>

#include "sampan/adpcm.h"
#include "sampan/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sampan_tx {
    uint16_t seq;
    struct sampan_adpcm adpcm[SAMPAN_MAX_CHANNELS];
};

void sampan_tx_init(struct sampan_tx *tx);

/* Encode `frames` interleaved samples as one framed audio packet into out
 * (SAMPAN_MAX_FRAME bytes is always enough). Returns bytes written, or 0 on
 * bad arguments. ADPCM state carries across calls and is also sent in each
 * packet, so a lost packet costs only its own audio. */
size_t sampan_tx_audio(struct sampan_tx *tx, uint8_t codec, uint8_t channels,
                       uint32_t rate, const int16_t *pcm, uint16_t frames,
                       uint8_t *out, size_t cap);

/* Any other packet type (MIDI, INFO). */
size_t sampan_tx_packet(struct sampan_tx *tx, uint8_t type, const uint8_t *body,
                        size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
