/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Sampler building blocks: tidy a recording (trim silence, even out the
 * level, fade the ends), play it back at any pitch, and a WAV header.
 * Mono, 16-bit.
 */
#ifndef SAMPAN_SAMPLE_H
#define SAMPAN_SAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The part of x[0..n) worth keeping: from `pre` samples before the first
 * sample above `threshold` to `post` samples after the last one. Returns the
 * kept length (0 if nothing is above the threshold); *start is its offset. */
size_t sampan_sample_trim(const int16_t *x, size_t n, int16_t threshold, size_t pre, size_t post,
                          size_t *start);

/* Scale so the peak reaches `target`, but by no more than `max_gain`.
 * Returns the gain applied. */
float sampan_sample_normalize(int16_t *x, size_t n, int16_t target, float max_gain);

/* Linear fade in over the first `in` samples and out over the last `out`. */
void sampan_sample_fade(int16_t *x, size_t n, size_t in, size_t out);

/* One playing voice with pitch and level. */
struct sampan_voice {
    const int16_t *data;
    size_t len;
    uint64_t pos;  /* 32.32 fixed point, in samples */
    uint64_t step; /* 32.32: 1.0 plays at the recorded pitch */
    int32_t gain;  /* Q15 */
    bool on;
};

/* Start from the beginning; `semitones` from the recorded pitch, gain 0..1. */
void sampan_voice_start(struct sampan_voice *v, const int16_t *data, size_t len, float semitones,
                        float gain);

/* Add n samples of the voice into out (saturating). Stops at the end. */
void sampan_voice_mix(struct sampan_voice *v, int16_t *out, size_t n);

/* 44-byte header of a 16-bit PCM WAV. */
void sampan_wav_header(uint8_t h[44], uint32_t rate, uint16_t channels, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
