/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Host-side tests for input conditioning and the level meter.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "sampan/dsp.h"

#define RATE 48000

static int16_t x[RATE * 2];

static void sine(double hz, double amp, double dc, size_t n)
{
    for (size_t i = 0; i < n; i++)
        x[i] = (int16_t)lrint(dc + amp * sin(2 * M_PI * hz * (double)i / RATE));
}

static double rms(const int16_t *s, size_t n)
{
    double a = 0;
    for (size_t i = 0; i < n; i++)
        a += (double)s[i] * s[i];
    return sqrt(a / (double)n);
}

static double mean(const int16_t *s, size_t n)
{
    double a = 0;
    for (size_t i = 0; i < n; i++)
        a += s[i];
    return a / (double)n;
}

/* Gain of the DC cut at hz, in dB, measured on the second half second. */
static double dccut_db(double hz, float cutoff)
{
    struct sampan_dccut d;
    struct sampan_gain g;
    sampan_dccut_init(&d, cutoff, RATE);
    sampan_gain_init(&g, 0, 1);
    sine(hz, 10000, 0, RATE);
    double in = rms(x + RATE / 2, RATE / 2);
    sampan_input_process(&d, &g, x, RATE);
    return 20 * log10(rms(x + RATE / 2, RATE / 2) / in);
}

static void test_dccut(void)
{
    struct sampan_dccut d;
    struct sampan_gain g;

    /* A big offset goes away; the tone stays. */
    sampan_dccut_init(&d, 20, RATE);
    sampan_gain_init(&g, 0, 1);
    sine(1000, 8000, 6000, RATE);
    sampan_input_process(&d, &g, x, RATE);
    double m = mean(x + RATE / 2, RATE / 2);
    printf("  dc cut: residual offset %.2f\n", m);
    CHECK(fabs(m) < 2);
    CHECK(g.clipped == 0);

    double pass = dccut_db(1000, 20), corner = dccut_db(20, 20), low = dccut_db(5, 20);
    printf("  dc cut 20 Hz: 1 kHz %.3f dB, 20 Hz %.2f dB, 5 Hz %.2f dB\n", pass, corner, low);
    CHECK(fabs(pass) < 0.05);
    CHECK(corner < -2 && corner > -4);
    CHECK(low < -10);

    /* Off: bit-exact pass-through. */
    sampan_dccut_init(&d, 0, RATE);
    sine(1000, 8000, 6000, 4800);
    static int16_t ref[4800];
    memcpy(ref, x, sizeof(ref));
    sampan_input_process(&d, &g, x, 4800);
    CHECK(memcmp(ref, x, sizeof(ref)) == 0);
    sampan_input_process(NULL, &g, x, 4800);
    CHECK(memcmp(ref, x, sizeof(ref)) == 0);
}

static void test_gain(void)
{
    struct sampan_gain g;
    sine(1000, 4000, 0, RATE);
    double in = rms(x, RATE);
    sampan_gain_init(&g, 6.0206f, 1);
    sampan_input_process(NULL, &g, x, RATE);
    CHECK(fabs(rms(x, RATE) / in - 2.0) < 0.001);
    CHECK(g.clipped == 0);

    /* Too much gain saturates and is counted, never wraps. */
    sine(1000, 4000, 0, RATE);
    sampan_gain_init(&g, 24, 1);
    sampan_input_process(NULL, &g, x, RATE);
    int wrap = 0;
    for (int i = 1; i < RATE; i++)
        if (abs(x[i] - x[i - 1]) > 40000)
            wrap = 1;
    CHECK(!wrap && g.clipped > RATE / 2);

    /* Changing gain ramps: on a steady DC level the output moves by no more
     * than the ramp step each sample, and lands on the target. */
    for (int i = 0; i < RATE; i++)
        x[i] = 1000;
    sampan_gain_init(&g, 0, 480);
    sampan_gain_set(&g, 20);
    sampan_input_process(NULL, &g, x, RATE);
    int max_step = 0;
    for (int i = 1; i < RATE; i++)
        if (abs(x[i] - x[i - 1]) > max_step)
            max_step = abs(x[i] - x[i - 1]);
    CHECK(max_step <= 20);         /* 9000 over 480 samples */
    CHECK(x[479] == 10000 && x[RATE - 1] == 10000);
    CHECK(x[0] > 1000 && x[0] < 1030);
}

static void test_meter(void)
{
    struct sampan_meter m;
    sampan_meter_init(&m, RATE, 1.0f, 20.0f);

    /* Full-scale sine: peak 0 dBFS, RMS -3 dBFS. */
    sine(1000, 32767, 0, 4800);
    sampan_meter_update(&m, x, 4800);
    CHECK(fabs(sampan_dbfs(m.peak)) < 0.01);
    CHECK(fabs(sampan_dbfs(m.rms) + 3.01) < 0.02);
    CHECK(m.clip == 1);

    /* Then silence: the peak holds for 1 s, then falls 20 dB per second. */
    memset(x, 0, sizeof(x));
    for (int k = 0; k < 10; k++)
        sampan_meter_update(&m, x, 4800); /* 1 s */
    CHECK(fabs(sampan_dbfs(m.peak)) < 0.01);
    CHECK(m.rms == 0 && m.clip == 0);
    for (int k = 0; k < 5; k++)
        sampan_meter_update(&m, x, 4800); /* 0.5 s more */
    printf("  meter: after 1 s hold + 0.5 s fall: %.2f dBFS\n", sampan_dbfs(m.peak));
    CHECK(fabs(sampan_dbfs(m.peak) + 10) < 0.1);

    /* A louder block resets the hold; quieter blocks don't push it down past them. */
    sine(1000, 3277, 0, 4800); /* -20 dBFS */
    for (int k = 0; k < 60; k++)
        sampan_meter_update(&m, x, 4800);
    CHECK(fabs(sampan_dbfs(m.peak) + 20) < 0.05);
    CHECK(sampan_dbfs(0) == -120.0f);
}

int main(void)
{
    test_dccut();
    test_gain();
    test_meter();
    return CHECK_DONE();
}
