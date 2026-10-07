/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Input conditioning for the microphone: DC cut, gain with a smooth ramp and
 * saturation, and a level meter. Float inside (the target cores have an FPU),
 * 16-bit in and out.
 */
#ifndef SAMPAN_DSP_H
#define SAMPAN_DSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-pole DC blocker: y = x - x1 + r * y1. */
struct sampan_dccut {
    float r, x1, y1;
};

/* Cutoff in Hz (about -3 dB there); 0 turns it into a pass-through. */
void sampan_dccut_init(struct sampan_dccut *d, float cutoff_hz, float rate);

/* Gain in dB, ramped over `ramp` samples when changed. */
struct sampan_gain {
    float cur, target, step;
    uint32_t left, ramp;
    uint32_t clipped; /* samples saturated so far */
};

void sampan_gain_init(struct sampan_gain *g, float db, uint32_t ramp_samples);
void sampan_gain_set(struct sampan_gain *g, float db);

/* In place, mono: DC cut (if d is not NULL), then gain and saturation. */
void sampan_input_process(struct sampan_dccut *d, struct sampan_gain *g, int16_t *x, size_t n);

/* Level meter: peak with hold then fall, and RMS of the last block. */
struct sampan_meter {
    float peak;          /* linear, 0..1, as shown (held / falling) */
    float rms;           /* linear, 0..1, last block */
    uint32_t hold_left;  /* samples until the peak starts to fall */
    uint32_t hold;       /* hold time in samples */
    float fall_per_sample; /* dB per sample */
    uint8_t clip;        /* the last block hit full scale */
};

void sampan_meter_init(struct sampan_meter *m, float rate, float hold_s, float fall_db_per_s);
void sampan_meter_update(struct sampan_meter *m, const int16_t *x, size_t n);

/* dBFS of a linear level, floored at -120. */
float sampan_dbfs(float lin);

#ifdef __cplusplus
}
#endif

#endif
