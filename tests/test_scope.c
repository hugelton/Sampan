/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Host-side tests for the waveform display.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "check.h"
#include "sampan/scope.h"

static uint32_t rng = 99;
static int16_t rnd16(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (int16_t)(rng >> 8);
}

static struct sampan_scope sc;
static struct sampan_scope_spans sp;

static void sine(int16_t *x, size_t n, double hz, double rate, double amp, double phase)
{
    for (size_t i = 0; i < n; i++)
        x[i] = (int16_t)lrint(amp * sin(2 * M_PI * hz * (double)i / rate + phase));
}

/* Column bounds, written independently of scope.c. */
static void bounds(uint32_t window, uint32_t width, uint32_t x, uint32_t *a, uint32_t *b)
{
    *a = (uint32_t)((uint64_t)x * window / width);
    *b = (uint32_t)((uint64_t)(x + 1) * window / width);
    if (*b <= *a)
        *b = *a + 1;
}

static void test_columns(void)
{
    static int16_t x[4000];
    const uint32_t windows[] = {1000, 128, 50, 1, 997};
    for (size_t k = 0; k < sizeof(windows) / sizeof(windows[0]); k++) {
        for (size_t i = 0; i < 4000; i++)
            x[i] = rnd16();
        struct sampan_scope_cfg cfg = {.width = 128, .window = windows[k], .trigger = false};
        sampan_scope_init(&sc, &cfg);
        /* Odd-sized pushes must not matter. */
        size_t off = 0, step = 1;
        while (off < windows[k]) {
            size_t n = step < windows[k] - off ? step : windows[k] - off;
            sampan_scope_push(&sc, x + off, n, 1);
            off += n;
            step = step * 3 % 17 + 1;
        }
        bool fresh;
        const struct sampan_scope_frame *f = sampan_scope_latest(&sc, &fresh);
        CHECK(f && fresh && f->seq == 1 && !f->triggered && f->width == 128);
        int ok = 1;
        for (uint32_t c = 0; f && c < 128; c++) {
            uint32_t a, b;
            bounds(windows[k], 128, c, &a, &b);
            int16_t lo = INT16_MAX, hi = INT16_MIN;
            for (uint32_t i = a; i < b; i++) {
                if (x[i] < lo)
                    lo = x[i];
                if (x[i] > hi)
                    hi = x[i];
            }
            if (f->lo[c] != lo || f->hi[c] != hi)
                ok = 0;
        }
        CHECK(ok);
    }

    /* Stereo: stride picks the first channel. */
    static int16_t st[2 * 256];
    for (int i = 0; i < 256; i++) {
        st[2 * i] = (int16_t)(i * 10);
        st[2 * i + 1] = -30000;
    }
    struct sampan_scope_cfg cfg = {.width = 256, .window = 256, .trigger = false};
    sampan_scope_init(&sc, &cfg);
    sampan_scope_push(&sc, st, 256, 2);
    const struct sampan_scope_frame *f = sampan_scope_latest(&sc, NULL);
    CHECK(f && f->lo[100] == 1000 && f->hi[255] == 2550);
}

static void test_trigger(void)
{
    const double rate = 48000;
    static int16_t x[48000];
    struct sampan_scope_cfg cfg;
    sampan_scope_cfg_default(&cfg, 128, (uint32_t)rate);
    CHECK(cfg.window == 960 && cfg.auto_samples == 4800);

    /* A steady tone, pushed in display-sized chunks: every frame starts on a
     * rising edge at the same phase. */
    sine(x, 48000, 440, rate, 12000, 1.0);
    sampan_scope_init(&sc, &cfg);
    int frames = 0, same = 1, edge = 1;
    int16_t first_hi[SAMPAN_SCOPE_MAX_W];
    for (size_t off = 0; off < 48000; off += 1600) {
        sampan_scope_push(&sc, x + off, 1600, 1);
        bool fresh;
        const struct sampan_scope_frame *f = sampan_scope_latest(&sc, &fresh);
        if (!fresh)
            continue;
        if (!f->triggered || f->lo[0] < 0 || f->lo[0] > 800)
            edge = 0;
        if (frames == 0)
            memcpy(first_hi, f->hi, sizeof(first_hi));
        else
            for (int c = 0; c < 128; c++)
                if (abs(f->hi[c] - first_hi[c]) > 800) /* one sample's worth of slope */
                    same = 0;
        frames++;
    }
    CHECK(frames == 30);
    CHECK(edge);
    CHECK(same);

    /* Noise smaller than the hysteresis never triggers; auto frames still come. */
    for (size_t i = 0; i < 48000; i++)
        x[i] = (int16_t)(rnd16() / 400); /* about +-80 */
    sampan_scope_init(&sc, &cfg);
    sampan_scope_push(&sc, x, 48000, 1);
    const struct sampan_scope_frame *f = sampan_scope_latest(&sc, NULL);
    CHECK(f && !f->triggered);
    /* auto after 4800 samples, then a 960-sample frame, repeated */
    CHECK(sc.seq == 48000 / (4800 + 960));

    /* Falling edges don't trigger; the frame starts at the next rising one. */
    sine(x, 4800, 100, rate, 10000, M_PI); /* starts going down */
    sampan_scope_init(&sc, &cfg);
    sampan_scope_push(&sc, x, 4800, 1);
    f = sampan_scope_latest(&sc, NULL);
    CHECK(f && f->triggered && f->lo[0] >= 0 && f->hi[5] > f->hi[0]);
}

static void test_triple_buffer(void)
{
    static int16_t x[10000];
    for (int i = 0; i < 10000; i++)
        x[i] = rnd16();
    struct sampan_scope_cfg cfg = {.width = 64, .window = 100, .trigger = false};
    sampan_scope_init(&sc, &cfg);
    bool fresh;
    CHECK(sampan_scope_latest(&sc, &fresh) == NULL && !fresh);

    uint32_t last = 0;
    int ok = 1;
    for (int round = 0; round < 50; round++) {
        size_t n = (size_t)(round % 7) * 100 + 37;
        sampan_scope_push(&sc, x, n, 1);
        const struct sampan_scope_frame *f = sampan_scope_latest(&sc, &fresh);
        if (f) {
            /* the newest finished frame, never the one being written */
            if (f->seq != sc.seq || f == &sc.frames[sc.back] || f->seq < last)
                ok = 0;
            if (fresh != (f->seq != last))
                ok = 0;
            last = f->seq;
        }
        const struct sampan_scope_frame *g = sampan_scope_latest(&sc, &fresh);
        if (g != f || fresh)
            ok = 0;
    }
    CHECK(ok);
    CHECK(last > 20);
}

static void push_frame(const int16_t *x, uint32_t window, uint16_t width)
{
    struct sampan_scope_cfg cfg = {.width = width, .window = window, .trigger = false};
    sampan_scope_init(&sc, &cfg);
    sampan_scope_push(&sc, x, window, 1);
}

static void test_spans_and_render(void)
{
    static int16_t x[2000];
    struct sampan_scope_style st;
    sampan_scope_style_default(&st, SAMPAN_PAL_AMBER);
    CHECK(st.bg == SAMPAN_BLACK && st.axis == sampan_palettes[1].c[0] && st.wave == sampan_palettes[1].c[4]);

    /* Random signal: the trace is connected and inside the screen. */
    for (int i = 0; i < 2000; i++)
        x[i] = rnd16();
    push_frame(x, 2000, 128);
    const struct sampan_scope_frame *f = sampan_scope_latest(&sc, NULL);
    sampan_scope_spans(f, 100, &st, &sp);
    int ok = 1;
    for (int c = 0; c < 128; c++) {
        if (sp.y0[c] > sp.y1[c] || sp.y1[c] >= 100)
            ok = 0;
        if (c && (sp.y0[c] > sp.y1[c - 1] + 1 || sp.y1[c] + 1 < sp.y0[c - 1]))
            ok = 0;
    }
    CHECK(ok);

    /* A steep tone (few samples per cycle) is still a connected line. */
    sine(x, 2000, 7000, 16000, 20000, 0);
    push_frame(x, 300, 128);
    f = sampan_scope_latest(&sc, NULL);
    sampan_scope_spans(f, 128, &st, &sp);
    ok = 1;
    for (int c = 1; c < 128; c++)
        if (sp.y0[c] > sp.y1[c - 1] + 1 || sp.y1[c] + 1 < sp.y0[c - 1])
            ok = 0;
    CHECK(ok);

    /* Zoomed in on big jumps both ways: each sample spans many columns and
     * the vertical joins carry the trace up and down. */
    for (int i = 0; i < 8; i++)
        x[i] = (int16_t)(i % 2 ? 20000 : -20000);
    push_frame(x, 8, 128);
    sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
    ok = 1;
    for (int c = 1; c < 128; c++)
        if (sp.y0[c] > sp.y1[c - 1] + 1 || sp.y1[c] + 1 < sp.y0[c - 1])
            ok = 0;
    CHECK(ok);
    CHECK(sp.y1[16] - sp.y0[16] > 50 && sp.y1[32] - sp.y0[32] > 50); /* the jumps */

    /* Auto scale: the peak reaches 44% of the height from the axis. */
    sine(x, 2000, 1000, 48000, 8000, 0);
    push_frame(x, 960, 128);
    f = sampan_scope_latest(&sc, NULL);
    sampan_scope_spans(f, 128, &st, &sp);
    int top = 128, bot = 0;
    for (int c = 0; c < 128; c++) {
        if (sp.y0[c] < top)
            top = sp.y0[c];
        if (sp.y1[c] > bot)
            bot = sp.y1[c];
    }
    CHECK(sp.axis_y == 64 && top == 64 - 56 && bot == 64 + 56);

    /* ... but quiet sounds aren't blown up past the floor. */
    sine(x, 2000, 1000, 48000, 750, 0);
    push_frame(x, 960, 128);
    sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
    top = 128;
    for (int c = 0; c < 128; c++)
        if (sp.y0[c] < top)
            top = sp.y0[c];
    CHECK(top == 64 - 28);

    /* Fixed gain: full scale at gain 1 fills the same 44%. */
    st.auto_scale = false;
    st.zoom_q8 = 256;
    sine(x, 2000, 1000, 48000, 32767, 0);
    push_frame(x, 960, 128);
    sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
    top = 128;
    for (int c = 0; c < 128; c++)
        if (sp.y0[c] < top)
            top = sp.y0[c];
    CHECK(top == 64 - 56 || top == 64 - 55);
    st.zoom_q8 = 1024; /* x4: clipped to the screen */
    sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
    int at_top = 0;
    for (int c = 0; c < 128; c++)
        at_top += sp.y0[c] == 0;
    CHECK(at_top > 10);
    st.auto_scale = true;

    /* Silence: a flat trace on the axis; everything else background. */
    memset(x, 0, sizeof(x));
    push_frame(x, 960, 128);
    sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
    static uint16_t fb[128 * 128], band[7 * 128];
    sampan_scope_render(&sp, &st, 0, 128, fb);
    int wave = 0, axis = 0, bg = 0;
    for (int i = 0; i < 128 * 128; i++) {
        wave += fb[i] == st.wave;
        axis += fb[i] == st.axis;
        bg += fb[i] == st.bg;
    }
    CHECK(wave == 128 && axis == 0 && bg == 128 * 127);
    CHECK(fb[64 * 128 + 5] == st.wave);

    /* Clipping shows in the accent colour. */
    for (int i = 0; i < 960; i++)
        x[i] = (i / 100) % 2 ? INT16_MAX : -1000;
    push_frame(x, 960, 128);
    f = sampan_scope_latest(&sc, NULL);
    sampan_scope_spans(f, 128, &st, &sp);
    sampan_scope_render(&sp, &st, 0, 128, fb);
    int clip_px = 0;
    for (int i = 0; i < 128 * 128; i++)
        clip_px += fb[i] == st.clip;
    CHECK(f->clip[20] && !f->clip[5] && clip_px > 0);

    /* Drawing in bands gives the same picture as drawing it whole. */
    for (int i = 0; i < 2000; i++)
        x[i] = rnd16();
    push_frame(x, 1500, 128);
    sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
    sampan_scope_render(&sp, &st, 0, 128, fb);
    ok = 1;
    for (int y = 0; y < 128; y += 7) {
        int rows = 128 - y < 7 ? 128 - y : 7;
        sampan_scope_render(&sp, &st, (uint16_t)y, (uint16_t)rows, band);
        if (memcmp(band, fb + y * 128, (size_t)rows * 128 * 2))
            ok = 0;
    }
    CHECK(ok);

    /* Byte swap for SPI panels. */
    st.swap_bytes = true;
    sampan_scope_render(&sp, &st, 0, 1, band);
    st.swap_bytes = false;
    ok = 1;
    for (int i = 0; i < 128; i++)
        if (band[i] != (uint16_t)(fb[i] >> 8 | fb[i] << 8))
            ok = 0;
    CHECK(ok);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static void measure(void)
{
    static int16_t x[48000];
    static uint16_t fb[128 * 128];
    sine(x, 48000, 440, 48000, 12000, 0);
    struct sampan_scope_cfg cfg;
    sampan_scope_cfg_default(&cfg, 128, 48000);
    sampan_scope_init(&sc, &cfg);
    struct sampan_scope_style st;
    sampan_scope_style_default(&st, SAMPAN_PAL_DEFAULT);
    double t0 = now_s();
    for (int k = 0; k < 20; k++)
        sampan_scope_push(&sc, x, 48000, 1);
    double t1 = now_s();
    for (int k = 0; k < 1000; k++) {
        sampan_scope_spans(sampan_scope_latest(&sc, NULL), 128, &st, &sp);
        sampan_scope_render(&sp, &st, 0, 128, fb);
    }
    double t2 = now_s();
    printf("  host: push %.1f ns/sample, spans+render 128x128 %.1f us/frame\n",
           (t1 - t0) / (20 * 48000) * 1e9, (t2 - t1) / 1000 * 1e6);
    printf("  RAM: struct sampan_scope %zu B, spans %zu B\n", sizeof(struct sampan_scope),
           sizeof(struct sampan_scope_spans));
}

int main(void)
{
    test_columns();
    test_trigger();
    test_triple_buffer();
    test_spans_and_render();
    measure();
    return CHECK_DONE();
}
