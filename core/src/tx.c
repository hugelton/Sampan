/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/tx.h"

#include <string.h>

/* Scratch buffers; static so the caller's task stack stays small. A single
 * sender is assumed. */
static uint8_t data_buf[SAMPAN_MAX_CHANNELS * SAMPAN_MAX_FRAMES * 2];
static uint8_t body_buf[SAMPAN_MAX_BODY]; /* PCM16 is the largest body */
static uint8_t packet_buf[SAMPAN_MAX_PACKET];

void sampan_tx_init(struct sampan_tx *tx)
{
    memset(tx, 0, sizeof(*tx));
}

size_t sampan_tx_packet(struct sampan_tx *tx, uint8_t type, const uint8_t *body,
                        size_t len, uint8_t *out, size_t cap)
{
    size_t n = sampan_packet_build(packet_buf, sizeof(packet_buf), type, tx->seq, body, len);
    if (n == 0)
        return 0;
    n = sampan_frame_encode(packet_buf, n, out, cap);
    if (n)
        tx->seq++;
    return n;
}

size_t sampan_tx_audio(struct sampan_tx *tx, uint8_t codec, uint8_t channels,
                       uint32_t rate, const int16_t *pcm, uint16_t frames,
                       uint8_t *out, size_t cap)
{
    size_t data_len = sampan_audio_data_len(codec, channels, frames);
    if (data_len == 0)
        return 0;

    struct sampan_audio a = {
        .codec = codec, .channels = channels, .rate = rate, .frames = frames,
    };
    if (codec == SAMPAN_CODEC_PCM16) {
        for (size_t i = 0; i < (size_t)channels * frames; i++) {
            data_buf[2 * i] = (uint8_t)pcm[i];
            data_buf[2 * i + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
        }
    } else {
        size_t per_ch = (frames + 1u) / 2;
        for (uint8_t c = 0; c < channels; c++) {
            a.adpcm_pred[c] = tx->adpcm[c].pred;
            a.adpcm_index[c] = tx->adpcm[c].index;
            sampan_adpcm_encode(&tx->adpcm[c], pcm + c, channels, frames,
                                data_buf + c * per_ch);
        }
    }

    size_t body_len = sampan_audio_build(body_buf, sizeof(body_buf), &a, data_buf, data_len);
    if (body_len == 0)
        return 0;
    return sampan_tx_packet(tx, SAMPAN_TYPE_AUDIO, body_buf, body_len, out, cap);
}
