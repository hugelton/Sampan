/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Sampan stream protocol: packets framed with COBS and a 0x00 delimiter,
 * so a receiver can join mid-stream and resync after any corruption.
 * The same framing runs over UART, USB CDC or a bulk pipe.
 *
 * Packet (before COBS), all multi-byte fields little-endian:
 *   [0]    version << 4 | type
 *   [1]    flags (reserved, 0)
 *   [2..3] seq
 *   [4..]  body
 *   [-2..] CRC-16/CCITT-FALSE over everything before it
 *
 * Audio body:
 *   [0]    codec            (SAMPAN_CODEC_*)
 *   [1]    channels         (interleaved)
 *   [2..5] sample rate, Hz
 *   [6..7] frames           (samples per channel in this packet)
 *   ADPCM only, per channel: predictor (i16), step index (u8), reserved (u8)
 *   then the sample data
 */
#ifndef SAMPAN_PROTO_H
#define SAMPAN_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAMPAN_PROTO_VERSION 1

enum sampan_type {
    SAMPAN_TYPE_AUDIO = 1,
    SAMPAN_TYPE_MIDI = 2, /* raw MIDI bytes */
    SAMPAN_TYPE_INFO = 3, /* UTF-8 text, e.g. firmware version */
};

enum sampan_codec {
    SAMPAN_CODEC_PCM16 = 0, /* signed 16-bit */
    SAMPAN_CODEC_ADPCM = 1, /* IMA ADPCM, 4 bits per sample, low nibble first */
};

#define SAMPAN_MAX_CHANNELS 2
#define SAMPAN_MAX_FRAMES 512 /* per packet, per channel */
#define SAMPAN_PACKET_HDR 4
#define SAMPAN_PACKET_CRC 2
#define SAMPAN_AUDIO_HDR 8
#define SAMPAN_ADPCM_CH_HDR 4
#define SAMPAN_MAX_BODY \
    (SAMPAN_AUDIO_HDR + SAMPAN_MAX_CHANNELS * SAMPAN_MAX_FRAMES * 2)
#define SAMPAN_MAX_PACKET (SAMPAN_PACKET_HDR + SAMPAN_MAX_BODY + SAMPAN_PACKET_CRC)
/* COBS adds one byte per 254 plus one; the frame adds the 0x00 delimiter. */
#define SAMPAN_COBS_MAX(n) ((n) + (n) / 254 + 1)
#define SAMPAN_MAX_FRAME (SAMPAN_COBS_MAX(SAMPAN_MAX_PACKET) + 1)

uint16_t sampan_crc16(const uint8_t *data, size_t len);

/* COBS without the delimiter. Return the output length, or 0 if it doesn't fit
 * (encode) or the input is malformed (decode). Encoding 0 bytes gives 1 byte;
 * decoding that byte gives 0, which callers treat as an error (packets are
 * never empty). */
size_t sampan_cobs_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);
size_t sampan_cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Packet with header and CRC; returns its length or 0 if it doesn't fit. */
size_t sampan_packet_build(uint8_t *out, size_t cap, uint8_t type, uint16_t seq,
                           const uint8_t *body, size_t body_len);

/* Packet -> COBS + 0x00, ready for the wire. Returns length or 0. */
size_t sampan_frame_encode(const uint8_t *packet, size_t len, uint8_t *out, size_t cap);

struct sampan_packet {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    const uint8_t *body;
    size_t body_len;
};

/* Checks length, version and CRC. Returns 0 or a negative SAMPAN_E_*. */
int sampan_packet_parse(const uint8_t *data, size_t len, struct sampan_packet *pkt);

enum {
    SAMPAN_OK = 0,
    SAMPAN_E_SHORT = -1,
    SAMPAN_E_CRC = -2,
    SAMPAN_E_VERSION = -3,
    SAMPAN_E_COBS = -4,
    SAMPAN_E_OVERFLOW = -5,
    SAMPAN_E_FORMAT = -6,
};

/* Byte-at-a-time receiver. */
struct sampan_rx {
    uint8_t enc[SAMPAN_MAX_FRAME];
    uint8_t dec[SAMPAN_MAX_PACKET];
    size_t len;
    bool overflow;
    uint32_t packets, crc_errors, cobs_errors, overflows, other_errors;
};

void sampan_rx_init(struct sampan_rx *rx);
/* Returns 1 with *pkt filled when a valid packet completes, 0 when more bytes
 * are needed, or a negative SAMPAN_E_* when a frame was dropped (counted in rx).
 * pkt->body points into rx and stays valid until the next push. */
int sampan_rx_push(struct sampan_rx *rx, uint8_t byte, struct sampan_packet *pkt);

/* Sequence tracking: returns how many packets were missed before seq. */
struct sampan_seq {
    bool have;
    uint16_t last;
    uint32_t lost;
};
uint32_t sampan_seq_update(struct sampan_seq *s, uint16_t seq);

struct sampan_audio {
    uint8_t codec;
    uint8_t channels;
    uint32_t rate;
    uint16_t frames;
    int16_t adpcm_pred[SAMPAN_MAX_CHANNELS];
    uint8_t adpcm_index[SAMPAN_MAX_CHANNELS];
    const uint8_t *data;
    size_t data_len;
};

/* Bytes of sample data for a given format, or 0 if the format is invalid. */
size_t sampan_audio_data_len(uint8_t codec, uint8_t channels, uint16_t frames);

/* Body from a->codec/channels/rate/frames/adpcm_* and `data`. Returns length or 0. */
size_t sampan_audio_build(uint8_t *out, size_t cap, const struct sampan_audio *a,
                          const uint8_t *data, size_t data_len);
int sampan_audio_parse(const uint8_t *body, size_t len, struct sampan_audio *a);

#ifdef __cplusplus
}
#endif

#endif
