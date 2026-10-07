/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Host-side tests for the sampler building blocks.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "sampan/sample.h"

static int16_t x[48000], out[96000];

static void test_trim(void)
{
    memset(x, 0, sizeof(x));
    for (int i = 1000; i < 3000; i++)
        x[i] = (int16_t)((i % 2) ? 5000 : -5000);
    x[500] = 50; /* below threshold: ignored */
    size_t start;
    size_t n = sampan_sample_trim(x, 48000, 200, 100, 300, &start);
    CHECK(start == 900 && n == 2000 + 100 + 300);

    /* Pre-roll and tail clamp at the ends. */
    n = sampan_sample_trim(x + 950, 2100, 200, 100, 300, &start);
    CHECK(start == 0 && n == 2100);

    memset(x, 0, sizeof(x));
    CHECK(sampan_sample_trim(x, 48000, 200, 100, 100, &start) == 0 && start == 0);
    x[0] = -1000;
    CHECK(sampan_sample_trim(x, 1, 200, 10, 10, &start) == 1);
}

static void test_normalize_fade(void)
{
    for (int i = 0; i < 1000; i++)
        x[i] = (int16_t)(i % 2 ? 4000 : -2000);
    float g = sampan_sample_normalize(x, 1000, 16000, 10.0f);
    CHECK(fabsf(g - 4.0f) < 1e-4f && x[1] == 16000 && x[0] == -8000);

    /* Quiet input: the gain stops at max_gain. */
    for (int i = 0; i < 1000; i++)
        x[i] = (int16_t)(i % 2 ? 100 : -100);
    g = sampan_sample_normalize(x, 1000, 16000, 10.0f);
    CHECK(g == 10.0f && x[1] == 1000);
    memset(x, 0, 2000);
    CHECK(sampan_sample_normalize(x, 1000, 16000, 10.0f) == 1.0f);

    for (int i = 0; i < 1000; i++)
        x[i] = 10000;
    sampan_sample_fade(x, 1000, 100, 200);
    CHECK(x[0] == 0 && x[50] == 5000 && x[100] == 10000 && x[999] == 0 && x[899] == 5000);
    CHECK(x[500] == 10000);
    sampan_sample_fade(x, 10, 100, 100); /* longer than the sample: no crash */
}

static void test_voice(void)
{
    for (int i = 0; i < 48000; i++)
        x[i] = (int16_t)lrint(10000 * sin(2 * M_PI * 440 * i / 48000.0));

    /* Original pitch, full gain: a copy (one sample short, as the last one
     * needs a neighbour to interpolate). */
    struct sampan_voice v;
    memset(out, 0, sizeof(out));
    sampan_voice_start(&v, x, 48000, 0, 1.0f);
    sampan_voice_mix(&v, out, 96000);
    int max_err = 0;
    for (int i = 0; i < 47999; i++)
        if (abs(out[i] - x[i]) > max_err)
            max_err = abs(out[i] - x[i]);
    CHECK(max_err <= 1);
    CHECK(!v.on && out[48000] == 0);

    /* An octave up: half as long, every other sample. */
    memset(out, 0, sizeof(out));
    sampan_voice_start(&v, x, 48000, 12, 1.0f);
    sampan_voice_mix(&v, out, 96000);
    int last = 0;
    for (int i = 0; i < 96000; i++)
        if (out[i])
            last = i;
    CHECK(last >= 23990 && last <= 24000);
    CHECK(abs(out[100] - x[200]) <= 1);

    /* An octave down at half gain: interpolated halfway between samples. */
    memset(out, 0, sizeof(out));
    sampan_voice_start(&v, x, 48000, -12, 0.5f);
    sampan_voice_mix(&v, out, 96000);
    CHECK(abs(out[201] - (x[100] + x[101]) / 4) <= 2);

    /* Mixing adds and saturates. */
    for (int i = 0; i < 100; i++)
        out[i] = 30000;
    int16_t loud[100];
    for (int i = 0; i < 100; i++)
        loud[i] = 20000;
    sampan_voice_start(&v, loud, 100, 0, 1.0f);
    sampan_voice_mix(&v, out, 100);
    CHECK(out[10] == 32767);

    /* Nothing to play. */
    sampan_voice_start(&v, NULL, 0, 0, 1.0f);
    CHECK(!v.on);
    sampan_voice_mix(&v, out, 10);
}

static void test_wav(void)
{
    uint8_t h[44];
    sampan_wav_header(h, 48000, 1, 12345);
    CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVEfmt ", 8) == 0);
    CHECK(h[24] == 0x80 && h[25] == 0xBB && h[22] == 1 && h[34] == 16);
    uint32_t data = (uint32_t)h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
    CHECK(data == 24690);
}

int main(void)
{
    test_trim();
    test_normalize_fade();
    test_voice();
    test_wav();
    return CHECK_DONE();
}
