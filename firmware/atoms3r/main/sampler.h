/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Minimal sampler: record into PSRAM, tidy the take and keep it on the FAT
 * volume as WAV, play the latest take at any pitch.
 */
#ifndef SAMPAN_SAMPLER_H
#define SAMPAN_SAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SAMPLER_RATE 48000
#define SAMPLER_MAX_S 10

/* Mount storage, allocate buffers, load the latest take. */
bool sampler_init(void);

/* Any task. */
void sampler_rec_start(void);
void sampler_rec_stop(void); /* the take is tidied and saved in the background */
void sampler_play(float semitones, float velocity);
void sampler_stop(void);
void sampler_set_volume(float v); /* 0..1, playback master */

/* Audio task: feed what the microphone hears; add playback into out. */
void sampler_input(const int16_t *x, size_t n);
void sampler_render(int16_t *out, size_t n);

struct sampler_status {
    bool ready, recording, playing, saving;
    uint32_t takes;         /* files saved so far */
    float last_s;           /* length of the take in memory */
    float rec_s;            /* recorded so far while recording */
    uint32_t free_kb;       /* on the volume */
    int last_error;         /* 0 or an esp_err_t from the last save */
    bool psram;
};
void sampler_status(struct sampler_status *s);

#endif
