/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/dsp.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

void sampan_dccut_init(struct sampan_dccut *d, float cutoff_hz, float rate)
{
    memset(d, 0, sizeof(*d));
    d->r = cutoff_hz > 0 ? expf(-6.2831853f * cutoff_hz / rate) : -1.0f;
}

static float db_to_lin(float db)
{
    return powf(10.0f, db / 20.0f);
}

void sampan_gain_init(struct sampan_gain *g, float db, uint32_t ramp_samples)
{
    memset(g, 0, sizeof(*g));
    g->cur = g->target = db_to_lin(db);
    g->ramp = ramp_samples ? ramp_samples : 1;
}

void sampan_gain_set(struct sampan_gain *g, float db)
{
    g->target = db_to_lin(db);
    g->left = g->ramp;
    g->step = (g->target - g->cur) / (float)g->ramp;
}

void sampan_input_process(struct sampan_dccut *d, struct sampan_gain *g, int16_t *x, size_t n)
{
    const bool dc = d && d->r >= 0;
    for (size_t i = 0; i < n; i++) {
        float v = (float)x[i];
        if (dc) {
            float y = v - d->x1 + d->r * d->y1;
            d->x1 = v;
            d->y1 = y;
            v = y;
        }
        if (g->left) {
            g->cur += g->step;
            if (--g->left == 0)
                g->cur = g->target;
        }
        v *= g->cur;
        if (v > 32767.0f) {
            v = 32767.0f;
            g->clipped++;
        } else if (v < -32768.0f) {
            v = -32768.0f;
            g->clipped++;
        }
        x[i] = (int16_t)lrintf(v);
    }
}

void sampan_meter_init(struct sampan_meter *m, float rate, float hold_s, float fall_db_per_s)
{
    memset(m, 0, sizeof(*m));
    m->hold = (uint32_t)(hold_s * rate);
    m->fall_per_sample = fall_db_per_s / rate;
}

void sampan_meter_update(struct sampan_meter *m, const int16_t *x, size_t n)
{
    int32_t pk = 0;
    double sum = 0;
    m->clip = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = x[i] < 0 ? -(int32_t)x[i] : x[i];
        if (a > pk)
            pk = a;
        if (x[i] == INT16_MAX || x[i] == INT16_MIN)
            m->clip = 1;
        sum += (double)x[i] * x[i];
    }
    m->rms = n ? (float)(sqrt(sum / (double)n) / 32768.0) : 0;

    float block_peak = (float)pk / 32768.0f;
    if (block_peak >= m->peak) {
        m->peak = block_peak;
        m->hold_left = m->hold;
        return;
    }
    /* Hold, then fall in dB for whatever is left of the block. */
    uint32_t k = (uint32_t)n;
    if (m->hold_left >= k) {
        m->hold_left -= k;
        return;
    }
    k -= m->hold_left;
    m->hold_left = 0;
    m->peak *= db_to_lin(-m->fall_per_sample * (float)k);
    if (m->peak < block_peak)
        m->peak = block_peak;
}

float sampan_dbfs(float lin)
{
    if (lin <= 1e-6f)
        return -120.0f;
    return 20.0f * log10f(lin);
}
