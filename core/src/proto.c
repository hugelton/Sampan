/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/proto.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16);
}

uint16_t sampan_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t sampan_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (cap < 1)
        return 0;
    size_t code_pos = 0, o = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < len; i++) {
        if (in[i] != 0) {
            if (o >= cap)
                return 0;
            out[o++] = in[i];
            code++;
        }
        if (in[i] == 0 || code == 0xFF) {
            out[code_pos] = code;
            if (o >= cap)
                return 0;
            code_pos = o++;
            code = 1;
        }
    }
    out[code_pos] = code;
    return o;
}

size_t sampan_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < len) {
        uint8_t code = in[i++];
        if (code == 0)
            return 0;
        for (uint8_t k = 1; k < code; k++) {
            if (i >= len || in[i] == 0 || o >= cap)
                return 0;
            out[o++] = in[i++];
        }
        if (code != 0xFF && i < len) {
            if (o >= cap)
                return 0;
            out[o++] = 0;
        }
    }
    return o;
}

size_t sampan_packet_build(uint8_t *out, size_t cap, uint8_t type, uint16_t seq,
                           const uint8_t *body, size_t body_len)
{
    size_t n = SAMPAN_PACKET_HDR + body_len + SAMPAN_PACKET_CRC;
    if (type > 0x0F || n > cap)
        return 0;
    out[0] = (uint8_t)(SAMPAN_PROTO_VERSION << 4 | type);
    out[1] = 0;
    put16(out + 2, seq);
    if (body_len)
        memcpy(out + SAMPAN_PACKET_HDR, body, body_len);
    put16(out + n - SAMPAN_PACKET_CRC, sampan_crc16(out, n - SAMPAN_PACKET_CRC));
    return n;
}

size_t sampan_frame_encode(const uint8_t *packet, size_t len, uint8_t *out, size_t cap)
{
    if (cap < 2)
        return 0;
    size_t n = sampan_cobs_encode(packet, len, out, cap - 1);
    if (n == 0)
        return 0;
    out[n++] = 0;
    return n;
}

int sampan_packet_parse(const uint8_t *data, size_t len, struct sampan_packet *pkt)
{
    if (len < SAMPAN_PACKET_HDR + SAMPAN_PACKET_CRC)
        return SAMPAN_E_SHORT;
    size_t body_end = len - SAMPAN_PACKET_CRC;
    if (get16(data + body_end) != sampan_crc16(data, body_end))
        return SAMPAN_E_CRC;
    if ((data[0] >> 4) != SAMPAN_PROTO_VERSION)
        return SAMPAN_E_VERSION;
    pkt->version = data[0] >> 4;
    pkt->type = data[0] & 0x0F;
    pkt->flags = data[1];
    pkt->seq = get16(data + 2);
    pkt->body = data + SAMPAN_PACKET_HDR;
    pkt->body_len = body_end - SAMPAN_PACKET_HDR;
    return SAMPAN_OK;
}

void sampan_rx_init(struct sampan_rx *rx)
{
    memset(rx, 0, sizeof(*rx));
}

int sampan_rx_push(struct sampan_rx *rx, uint8_t byte, struct sampan_packet *pkt)
{
    if (byte != 0) {
        if (rx->overflow)
            return 0;
        if (rx->len >= sizeof(rx->enc)) {
            rx->overflow = true;
            return 0;
        }
        rx->enc[rx->len++] = byte;
        return 0;
    }

    size_t len = rx->len;
    bool overflow = rx->overflow;
    rx->len = 0;
    rx->overflow = false;
    if (overflow) {
        rx->overflows++;
        return SAMPAN_E_OVERFLOW;
    }
    if (len == 0)
        return 0; /* idle delimiters */

    size_t n = sampan_cobs_decode(rx->enc, len, rx->dec, sizeof(rx->dec));
    if (n == 0) {
        rx->cobs_errors++;
        return SAMPAN_E_COBS;
    }
    int r = sampan_packet_parse(rx->dec, n, pkt);
    if (r == SAMPAN_E_CRC)
        rx->crc_errors++;
    else if (r < 0)
        rx->other_errors++;
    else
        rx->packets++;
    return r < 0 ? r : 1;
}

uint32_t sampan_seq_update(struct sampan_seq *s, uint16_t seq)
{
    if (!s->have) {
        s->have = true;
        s->last = seq;
        return 0;
    }
    uint16_t gap = (uint16_t)(seq - s->last - 1);
    s->last = seq;
    /* A huge gap means a duplicate, a reorder or a sender restart, not loss. */
    if (gap >= 0x8000)
        return 0;
    s->lost += gap;
    return gap;
}

size_t sampan_audio_data_len(uint8_t codec, uint8_t channels, uint16_t frames)
{
    if (channels < 1 || channels > SAMPAN_MAX_CHANNELS || frames < 1 ||
        frames > SAMPAN_MAX_FRAMES)
        return 0;
    switch (codec) {
    case SAMPAN_CODEC_PCM16:
        return (size_t)channels * frames * 2;
    case SAMPAN_CODEC_ADPCM:
        return (size_t)channels * ((frames + 1u) / 2);
    default:
        return 0;
    }
}

static size_t audio_hdr_len(uint8_t codec, uint8_t channels)
{
    return SAMPAN_AUDIO_HDR +
           (codec == SAMPAN_CODEC_ADPCM ? (size_t)channels * SAMPAN_ADPCM_CH_HDR : 0);
}

static bool rate_ok(uint32_t rate)
{
    return rate >= 1000 && rate <= 192000;
}

size_t sampan_audio_build(uint8_t *out, size_t cap, const struct sampan_audio *a,
                          const uint8_t *data, size_t data_len)
{
    size_t want = sampan_audio_data_len(a->codec, a->channels, a->frames);
    if (want == 0 || data_len != want || !rate_ok(a->rate))
        return 0;
    size_t hdr = audio_hdr_len(a->codec, a->channels);
    if (hdr + data_len > cap)
        return 0;
    out[0] = a->codec;
    out[1] = a->channels;
    put32(out + 2, a->rate);
    put16(out + 6, a->frames);
    if (a->codec == SAMPAN_CODEC_ADPCM) {
        for (uint8_t c = 0; c < a->channels; c++) {
            uint8_t *p = out + SAMPAN_AUDIO_HDR + c * SAMPAN_ADPCM_CH_HDR;
            put16(p, (uint16_t)a->adpcm_pred[c]);
            p[2] = a->adpcm_index[c];
            p[3] = 0;
        }
    }
    memcpy(out + hdr, data, data_len);
    return hdr + data_len;
}

int sampan_audio_parse(const uint8_t *body, size_t len, struct sampan_audio *a)
{
    if (len < SAMPAN_AUDIO_HDR)
        return SAMPAN_E_SHORT;
    memset(a, 0, sizeof(*a));
    a->codec = body[0];
    a->channels = body[1];
    a->rate = get32(body + 2);
    a->frames = get16(body + 6);
    size_t want = sampan_audio_data_len(a->codec, a->channels, a->frames);
    if (want == 0 || !rate_ok(a->rate))
        return SAMPAN_E_FORMAT;
    size_t hdr = audio_hdr_len(a->codec, a->channels);
    if (len != hdr + want)
        return SAMPAN_E_FORMAT;
    if (a->codec == SAMPAN_CODEC_ADPCM) {
        for (uint8_t c = 0; c < a->channels; c++) {
            const uint8_t *p = body + SAMPAN_AUDIO_HDR + c * SAMPAN_ADPCM_CH_HDR;
            a->adpcm_pred[c] = (int16_t)get16(p);
            a->adpcm_index[c] = p[2];
            if (p[2] > 88)
                return SAMPAN_E_FORMAT;
        }
    }
    a->data = body + hdr;
    a->data_len = want;
    return SAMPAN_OK;
}
