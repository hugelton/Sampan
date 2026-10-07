/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Host-side tests for the portable core.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sampan/adpcm.h"
#include "sampan/proto.h"
#include "sampan/ring.h"
#include "sampan/tx.h"
#include "check.h"
#include "ima4_sweep.h"

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void test_crc(void)
{
    /* CRC-16/CCITT-FALSE check value */
    CHECK(sampan_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
    CHECK(sampan_crc16(NULL, 0) == 0xFFFF);
}

static void cobs_roundtrip(const uint8_t *in, size_t n)
{
    static uint8_t enc[4096], dec[4096];
    size_t e = sampan_cobs_encode(in, n, enc, sizeof(enc));
    CHECK(e > 0);
    CHECK(e <= SAMPAN_COBS_MAX(n));
    CHECK(memchr(enc, 0, e) == NULL);
    size_t d = sampan_cobs_decode(enc, e, dec, sizeof(dec));
    CHECK(d == n);
    CHECK(n == 0 || memcmp(in, dec, n) == 0);
}

static void test_cobs(void)
{
    static uint8_t buf[2048];
    cobs_roundtrip(buf, 0);

    memset(buf, 0, sizeof(buf));
    cobs_roundtrip(buf, 1);
    cobs_roundtrip(buf, 300);

    /* Block boundaries around 254 non-zero bytes. */
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i % 255 + 1);
    for (size_t n = 250; n <= 260; n++)
        cobs_roundtrip(buf, n);
    cobs_roundtrip(buf, 508);
    cobs_roundtrip(buf, 509);

    for (int t = 0; t < 500; t++) {
        size_t n = rnd() % 1500;
        for (size_t i = 0; i < n; i++)
            buf[i] = (rnd() % 4) ? (uint8_t)rnd() : 0;
        cobs_roundtrip(buf, n);
    }

    /* Too small an output buffer fails cleanly. */
    uint8_t small[4];
    CHECK(sampan_cobs_encode(buf, 10, small, sizeof(small)) == 0);

    /* Malformed input: a code that runs past the end, an embedded zero. */
    const uint8_t bad1[] = {0x05, 0x11, 0x22};
    const uint8_t bad2[] = {0x03, 0x11, 0x00};
    uint8_t out[16];
    CHECK(sampan_cobs_decode(bad1, sizeof(bad1), out, sizeof(out)) == 0);
    CHECK(sampan_cobs_decode(bad2, sizeof(bad2), out, sizeof(out)) == 0);
}

static void test_packet(void)
{
    uint8_t body[] = {1, 2, 0, 3};
    uint8_t pkt[64];
    size_t n = sampan_packet_build(pkt, sizeof(pkt), SAMPAN_TYPE_MIDI, 0xBEEF, body, sizeof(body));
    CHECK(n == SAMPAN_PACKET_HDR + sizeof(body) + SAMPAN_PACKET_CRC);

    struct sampan_packet p;
    CHECK(sampan_packet_parse(pkt, n, &p) == SAMPAN_OK);
    CHECK(p.type == SAMPAN_TYPE_MIDI && p.seq == 0xBEEF && p.version == 1);
    CHECK(p.body_len == sizeof(body) && memcmp(p.body, body, sizeof(body)) == 0);

    /* Every single-bit flip is caught. */
    int caught = 0, total = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            pkt[i] ^= (uint8_t)(1 << b);
            total++;
            caught += sampan_packet_parse(pkt, n, &p) < 0;
            pkt[i] ^= (uint8_t)(1 << b);
        }
    CHECK(caught == total);

    CHECK(sampan_packet_parse(pkt, 5, &p) == SAMPAN_E_SHORT);
    CHECK(sampan_packet_build(pkt, 8, SAMPAN_TYPE_MIDI, 0, body, sizeof(body)) == 0);
    CHECK(sampan_packet_build(pkt, sizeof(pkt), 0x10, 0, body, sizeof(body)) == 0);
}

static int push_all(struct sampan_rx *rx, const uint8_t *d, size_t n,
                    struct sampan_packet *last, int *errors)
{
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        int r = sampan_rx_push(rx, d[i], last);
        if (r == 1)
            got++;
        else if (r < 0)
            (*errors)++;
    }
    return got;
}

static void test_rx(void)
{
    static struct sampan_rx rx;
    static uint8_t wire[8192];
    struct sampan_tx tx;
    sampan_tx_init(&tx);
    sampan_rx_init(&rx);

    const uint8_t info[] = "Sampan";
    size_t n = 0;
    for (int i = 0; i < 5; i++)
        n += sampan_tx_packet(&tx, SAMPAN_TYPE_INFO, info, 6, wire + n, sizeof(wire) - n);

    struct sampan_packet p;
    int errors = 0;
    /* Join mid-frame: the partial first frame is dropped, the rest arrive. */
    int got = push_all(&rx, wire + 3, n - 3, &p, &errors);
    CHECK(got == 4);
    CHECK(errors == 1);
    CHECK(p.seq == 4 && p.body_len == 6 && memcmp(p.body, "Sampan", 6) == 0);

    /* Garbage between frames, idle zeros and a corrupted frame. */
    sampan_rx_init(&rx);
    uint8_t noise[] = {0x00, 0x00, 0x13, 0x37, 0xFF, 0x00, 0x00};
    errors = 0;
    got = push_all(&rx, noise, sizeof(noise), &p, &errors);
    CHECK(got == 0 && errors == 1);
    size_t one = sampan_tx_packet(&tx, SAMPAN_TYPE_INFO, info, 6, wire, sizeof(wire));
    wire[2] ^= 0x40;
    errors = 0;
    CHECK(push_all(&rx, wire, one, &p, &errors) == 0);
    CHECK(errors == 1 && rx.crc_errors + rx.cobs_errors >= 1);
    wire[2] ^= 0x40;
    CHECK(push_all(&rx, wire, one, &p, &errors) == 1);

    /* An endless run without a delimiter overflows once, then recovers. */
    sampan_rx_init(&rx);
    errors = 0;
    for (size_t i = 0; i < SAMPAN_MAX_FRAME + 100; i++)
        CHECK(sampan_rx_push(&rx, 0x55, &p) == 0);
    CHECK(sampan_rx_push(&rx, 0x00, &p) == SAMPAN_E_OVERFLOW);
    CHECK(push_all(&rx, wire, one, &p, &errors) == 1 && errors == 0);
}

static void test_seq(void)
{
    struct sampan_seq s = {0};
    CHECK(sampan_seq_update(&s, 65534) == 0);
    CHECK(sampan_seq_update(&s, 65535) == 0);
    CHECK(sampan_seq_update(&s, 0) == 0); /* wrap */
    CHECK(sampan_seq_update(&s, 3) == 2);
    CHECK(sampan_seq_update(&s, 3) == 0); /* duplicate */
    CHECK(s.lost == 2);
}

static void test_audio_body(void)
{
    uint8_t data[8] = {0};
    uint8_t body[64];
    struct sampan_audio a = {.codec = SAMPAN_CODEC_PCM16, .channels = 1, .rate = 16000, .frames = 4};
    size_t n = sampan_audio_build(body, sizeof(body), &a, data, 8);
    CHECK(n == SAMPAN_AUDIO_HDR + 8);

    struct sampan_audio b;
    CHECK(sampan_audio_parse(body, n, &b) == SAMPAN_OK);
    CHECK(b.rate == 16000 && b.frames == 4 && b.data_len == 8);
    CHECK(sampan_audio_parse(body, n - 1, &b) == SAMPAN_E_FORMAT);
    CHECK(sampan_audio_parse(body, 3, &b) == SAMPAN_E_SHORT);

    CHECK(sampan_audio_build(body, sizeof(body), &a, data, 7) == 0);
    a.rate = 0;
    CHECK(sampan_audio_build(body, sizeof(body), &a, data, 8) == 0);
    a.rate = 16000;
    a.channels = 3;
    CHECK(sampan_audio_build(body, sizeof(body), &a, data, 8) == 0);

    /* ADPCM: odd frame count rounds up; a bad step index is rejected. */
    CHECK(sampan_audio_data_len(SAMPAN_CODEC_ADPCM, 1, 5) == 3);
    CHECK(sampan_audio_data_len(SAMPAN_CODEC_ADPCM, 2, 5) == 6);
    CHECK(sampan_audio_data_len(SAMPAN_CODEC_PCM16, 1, SAMPAN_MAX_FRAMES + 1) == 0);
    struct sampan_audio c = {.codec = SAMPAN_CODEC_ADPCM, .channels = 1, .rate = 48000,
                             .frames = 5, .adpcm_pred = {-1234}, .adpcm_index = {40}};
    n = sampan_audio_build(body, sizeof(body), &c, data, 3);
    CHECK(n == SAMPAN_AUDIO_HDR + SAMPAN_ADPCM_CH_HDR + 3);
    CHECK(sampan_audio_parse(body, n, &b) == SAMPAN_OK);
    CHECK(b.adpcm_pred[0] == -1234 && b.adpcm_index[0] == 40);
    body[SAMPAN_AUDIO_HDR + 2] = 89;
    CHECK(sampan_audio_parse(body, n, &b) == SAMPAN_E_FORMAT);
}

static double snr_db(const int16_t *ref, const int16_t *got, size_t n)
{
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)ref[i] - got[i];
        sig += (double)ref[i] * ref[i];
        err += d * d;
    }
    return err == 0 ? 999 : 10 * log10(sig / err);
}

static void make_sine(int16_t *out, size_t n, double hz, double rate, double amp)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (int16_t)lrint(amp * sin(2 * M_PI * hz * (double)i / rate));
}

/* Known answers from an independent decoder: macOS IMA4 packets (64
 * samples) encoded and decoded by afconvert, with the decoder state carried
 * in from the previous packets. A 1234 Hz tone from silence (the step size
 * climbing from the bottom of the table) and later on, and a 7900 Hz tone,
 * all at 16 kHz. */
static const struct {
    int16_t pred;
    uint8_t index;
    uint8_t data[32];
    int16_t out[64];
} adpcm_kat[] = {
    {0, 0,
     {0x70, 0x77, 0x77, 0xF7, 0xFF, 0x19, 0x42, 0x23, 0x81, 0xDA, 0xCB, 0x89,
      0x31, 0x35, 0x12, 0xB8, 0xBD, 0xAB, 0x18, 0x45, 0x23, 0x91, 0xDA, 0xCB,
      0x89, 0x41, 0x43, 0x02, 0xA8, 0xBD, 0x9B, 0x28},
     {0, 11, 41, 104, 240, 533, 1164, -193, -3103, -9339,
      -12013, -9582, -5899, 128, 5801, 9484, 11492, 10884, 8117, 2582,
      -2574, -8601, -11032, -11768, -9760, -5500, 588, 6261, 9944, 11952,
      11344, 7470, 1935, -3221, -7908, -10951, -11504, -9995, -4963, 1064,
      6737, 10420, 12428, 10603, 7836, 2301, -2855, -8882, -11313, -12049,
      -10041, -4562, 594, 6621, 10673, 11409, 10740, 7697, 1609, -4064,
      -9220, -11228, -11836, -9069}},
    {-12419, 69,
     {0x30, 0x44, 0x22, 0x90, 0xBC, 0xAD, 0x08, 0x32, 0x35, 0x01, 0xB9, 0xAE,
      0x9A, 0x20, 0x44, 0x13, 0x90, 0xCC, 0xAB, 0x19, 0x52, 0x43, 0x01, 0xC9,
      0xCB, 0x8A, 0x20, 0x35, 0x13, 0xA0, 0xCC, 0xBB},
     {-11750, -7490, -2509, 3518, 7570, 11253, 11922, 10097, 5116, 429,
      -6267, -10724, -11534, -10798, -7450, -3190, 2898, 8571, 10780, 11449,
      9624, 5750, -792, -5249, -9301, -11510, -10841, -7798, -2817, 3210,
      8883, 11092, 11761, 9936, 4955, -1072, -6745, -10428, -12436, -10611,
      -7844, -2309, 2847, 8874, 11305, 12041, 10033, 4554, -602, -6629,
      -10681, -11417, -10748, -7705, -1617, 4056, 9212, 11220, 11828, 9061,
      4532, -947, -6103, -10790}},
    {-907, 68,
     {0x00, 0x29, 0x5B, 0x4C, 0x4C, 0x3B, 0x4C, 0x3C, 0x4B, 0x3C, 0x4B, 0x3C,
      0x4B, 0x3B, 0x4C, 0x3B, 0x3C, 0x4B, 0x3B, 0x3C, 0x4B, 0x3B, 0x3C, 0x4B,
      0x3B, 0x3C, 0x4B, 0x3B, 0x3B, 0x3C, 0x3B, 0x4B},
     {-299, 254, -1255, 1032, -1877, 2281, -2700, 3327, -3967, 4858,
      -3447, 4103, -4722, 5957, -6965, 5195, -5859, 7063, -8573, 6142,
      -7235, 8401, -10519, 7286, -8901, 10019, -7786, 8401, -10519, 12374,
      -9169, 10417, -12476, 9067, -10519, 12374, -9169, 10417, -12476, 9067,
      -10519, 12374, -9169, 10417, -12476, 9067, -10519, 12374, -9169, 10417,
      -12476, 9067, -10519, 12374, -9169, 10417, -7388, 8799, -10121, 7684,
      -8503, 6212, -7165, 8471}},
};

static void test_adpcm_known(void)
{
    for (size_t k = 0; k < sizeof(adpcm_kat) / sizeof(adpcm_kat[0]); k++) {
        struct sampan_adpcm st = {adpcm_kat[k].pred, adpcm_kat[k].index};
        int16_t out[64];
        sampan_adpcm_decode(&st, adpcm_kat[k].data, 64, out, 1);
        CHECK(memcmp(out, adpcm_kat[k].out, sizeof(out)) == 0);
    }
}

/* The made-up stream in ima4_sweep.h covers all 89 step indices, all 16
 * nibbles and clipping. The hash is of afconvert's decode of it (macOS,
 * 2026-10-06), which matched ours sample for sample. */
static void test_adpcm_sweep(void)
{
    static uint8_t pk[IMA4_SWEEP_PACKETS][34];
    static int16_t out[IMA4_SWEEP_PACKETS * 64];
    ima4_sweep_make(pk);
    ima4_sweep_decode(pk, out);
    CHECK(ima4_sweep_hash(out, IMA4_SWEEP_PACKETS * 64) == 0x812608a0263a1dedu);
}

static void test_adpcm(void)
{
    enum { N = 16000 };
    static int16_t in[N], out[N];
    static uint8_t enc[N / 2];

    make_sine(in, N, 440, 16000, 16000);
    struct sampan_adpcm e, d;
    sampan_adpcm_reset(&e);
    sampan_adpcm_reset(&d);
    sampan_adpcm_encode(&e, in, 1, N, enc);
    sampan_adpcm_decode(&d, enc, N, out, 1);
    /* The encoder tracks the decoder exactly. */
    CHECK(e.pred == d.pred && e.index == d.index);
    /* Skip the start-up while the step size adapts. */
    double snr = snr_db(in + 200, out + 200, N - 200);
    printf("  adpcm 440 Hz @ 16 kHz: SNR %.1f dB\n", snr);
    CHECK(snr > 25);

    /* Full-scale square wave: clamping, no wrap-around. */
    for (size_t i = 0; i < N; i++)
        in[i] = ((i / 40) & 1) ? 32767 : -32768;
    sampan_adpcm_reset(&e);
    sampan_adpcm_reset(&d);
    sampan_adpcm_encode(&e, in, 1, N, enc);
    sampan_adpcm_decode(&d, enc, N, out, 1);
    int sign_ok = 1;
    for (size_t i = 0; i < N; i++)
        if ((i % 40) > 30 && ((in[i] > 0) != (out[i] > 0)))
            sign_ok = 0;
    CHECK(sign_ok);

    /* Splitting into chunks gives the same bytes as one call (even sizes). */
    static uint8_t enc2[N / 2];
    make_sine(in, N, 1000, 16000, 8000);
    sampan_adpcm_reset(&e);
    sampan_adpcm_encode(&e, in, 1, N, enc);
    sampan_adpcm_reset(&e);
    for (size_t off = 0; off < N; off += 256)
        sampan_adpcm_encode(&e, in + off, 1, N - off < 256 ? N - off : 256, enc2 + off / 2);
    CHECK(memcmp(enc, enc2, sizeof(enc)) == 0);
}

static void test_ring(void)
{
    uint8_t store[64];
    struct sampan_ring r;
    CHECK(!sampan_ring_init(&r, store, 48));
    CHECK(sampan_ring_init(&r, store, sizeof(store)));
    CHECK(sampan_ring_free(&r) == 64);

    uint8_t src[200], dst[200];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)i;
    CHECK(sampan_ring_write(&r, src, 100) == 64);
    CHECK(sampan_ring_write(&r, src, 1) == 0);
    CHECK(sampan_ring_read(&r, dst, 40) == 40);
    CHECK(memcmp(dst, src, 40) == 0);

    /* Wrap-around with odd sizes, checked against a running counter. */
    sampan_ring_init(&r, store, sizeof(store));
    uint8_t wv = 0, rv = 0;
    int ok = 1;
    for (int t = 0; t < 10000; t++) {
        size_t n = rnd() % 50;
        for (size_t i = 0; i < n; i++)
            src[i] = (uint8_t)(wv + i);
        wv = (uint8_t)(wv + sampan_ring_write(&r, src, n));
        n = sampan_ring_read(&r, dst, rnd() % 50);
        for (size_t i = 0; i < n; i++)
            if (dst[i] != rv++)
                ok = 0;
    }
    CHECK(ok);
    CHECK(sampan_ring_used(&r) == (uint8_t)(wv - rv));
}

/* Sender -> wire with dropped and corrupted frames -> receiver. */
static void test_end_to_end(void)
{
    enum { FRAMES = 256, PACKETS = 200 };
    static int16_t pcm[FRAMES * PACKETS], got[FRAMES * PACKETS];
    static uint8_t frame[SAMPAN_MAX_FRAME];
    static struct sampan_rx rx;
    make_sine(pcm, FRAMES * PACKETS, 523.25, 48000, 12000);

    for (int codec = 0; codec <= 1; codec++) {
        struct sampan_tx tx;
        struct sampan_seq seq = {0};
        sampan_tx_init(&tx);
        sampan_rx_init(&rx);
        memset(got, 0, sizeof(got));
        int received = 0;

        for (int k = 0; k < PACKETS; k++) {
            size_t n = sampan_tx_audio(&tx, (uint8_t)codec, 1, 48000, pcm + k * FRAMES,
                                       FRAMES, frame, sizeof(frame));
            CHECK(n > 0);
            if (k % 17 == 5)
                continue; /* dropped */
            if (k % 23 == 7)
                frame[n / 2] ^= 0x01; /* corrupted */
            struct sampan_packet p;
            for (size_t i = 0; i < n; i++) {
                if (sampan_rx_push(&rx, frame[i], &p) != 1)
                    continue;
                struct sampan_audio a;
                CHECK(sampan_audio_parse(p.body, p.body_len, &a) == SAMPAN_OK);
                CHECK(a.frames == FRAMES && a.rate == 48000);
                sampan_seq_update(&seq, p.seq);
                int16_t *dst = got + (size_t)p.seq * FRAMES;
                if (a.codec == SAMPAN_CODEC_PCM16) {
                    for (size_t j = 0; j < FRAMES; j++)
                        dst[j] = (int16_t)(a.data[2 * j] | a.data[2 * j + 1] << 8);
                } else {
                    struct sampan_adpcm st = {a.adpcm_pred[0], a.adpcm_index[0]};
                    sampan_adpcm_decode(&st, a.data, FRAMES, dst, 1);
                }
                received++;
            }
        }
        int dropped = 0, corrupted = 0;
        for (int k = 0; k < PACKETS; k++) {
            if (k % 17 == 5)
                dropped++;
            else if (k % 23 == 7)
                corrupted++;
        }
        CHECK(received == PACKETS - dropped - corrupted);
        CHECK(seq.lost == (uint32_t)(dropped + corrupted));
        CHECK(rx.crc_errors + rx.cobs_errors == (uint32_t)corrupted);

        /* Packets that arrived decode on their own, even right after a gap. */
        int bad = 0;
        for (int k = 1; k < PACKETS; k++) {
            if (k % 17 == 5 || k % 23 == 7)
                continue;
            const int16_t *ref = pcm + k * FRAMES, *out = got + k * FRAMES;
            double s = snr_db(ref, out, FRAMES);
            if (s < (codec ? 20 : 999))
                bad++;
        }
        CHECK(bad == 0);
    }
}

int main(void)
{
    test_crc();
    test_cobs();
    test_packet();
    test_rx();
    test_seq();
    test_audio_body();
    test_adpcm();
    test_adpcm_known();
    test_adpcm_sweep();
    test_ring();
    test_end_to_end();
    return CHECK_DONE();
}
