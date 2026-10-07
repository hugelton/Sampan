/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/sample.h"

#include <math.h>
#include <string.h>

static int abs16(int16_t v)
{
    return v < 0 ? -(int)v : v;
}

size_t sampan_sample_trim(const int16_t *x, size_t n, int16_t threshold, size_t pre, size_t post,
                          size_t *start)
{
    size_t first = n, last = 0;
    for (size_t i = 0; i < n; i++)
        if (abs16(x[i]) > threshold) {
            first = i;
            break;
        }
    *start = 0;
    if (first == n)
        return 0;
    for (size_t i = n; i-- > first;)
        if (abs16(x[i]) > threshold) {
            last = i;
            break;
        }
    size_t a = first > pre ? first - pre : 0;
    size_t b = last + 1 + post < n ? last + 1 + post : n;
    *start = a;
    return b - a;
}

float sampan_sample_normalize(int16_t *x, size_t n, int16_t target, float max_gain)
{
    int peak = 0;
    for (size_t i = 0; i < n; i++)
        if (abs16(x[i]) > peak)
            peak = abs16(x[i]);
    if (peak == 0)
        return 1.0f;
    float g = (float)target / (float)peak;
    if (g > max_gain)
        g = max_gain;
    for (size_t i = 0; i < n; i++) {
        float v = x[i] * g;
        x[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
    }
    return g;
}

void sampan_sample_fade(int16_t *x, size_t n, size_t in, size_t out)
{
    if (in > n)
        in = n;
    if (out > n)
        out = n;
    for (size_t i = 0; i < in; i++)
        x[i] = (int16_t)((int32_t)x[i] * (int32_t)i / (int32_t)in);
    for (size_t i = 0; i < out; i++)
        x[n - 1 - i] = (int16_t)((int32_t)x[n - 1 - i] * (int32_t)i / (int32_t)out);
}

void sampan_voice_start(struct sampan_voice *v, const int16_t *data, size_t len, float semitones,
                        float gain)
{
    v->data = data;
    v->len = len;
    v->pos = 0;
    v->step = (uint64_t)llround(exp2((double)semitones / 12.0) * 4294967296.0);
    if (gain < 0)
        gain = 0;
    if (gain > 1)
        gain = 1;
    v->gain = (int32_t)lrintf(gain * 32767.0f);
    v->on = data && len > 1 && v->step > 0;
}

void sampan_voice_mix(struct sampan_voice *v, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n && v->on; i++) {
        size_t k = (size_t)(v->pos >> 32);
        if (k + 1 >= v->len) {
            v->on = false;
            break;
        }
        int32_t frac = (int32_t)((v->pos >> 17) & 0x7FFF); /* Q15 */
        int32_t a = v->data[k], b = v->data[k + 1];
        int32_t s = a + (((b - a) * frac) >> 15);
        int32_t m = out[i] + ((s * v->gain) >> 15);
        out[i] = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
        v->pos += v->step;
    }
}

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, (uint16_t)v);
    le16(p + 2, (uint16_t)(v >> 16));
}

void sampan_wav_header(uint8_t h[44], uint32_t rate, uint16_t ch, uint32_t frames)
{
    uint32_t data = frames * ch * 2u;
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);
    le16(h + 22, ch);
    le32(h + 24, rate);
    le32(h + 28, rate * ch * 2u);
    le16(h + 32, (uint16_t)(ch * 2));
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}
