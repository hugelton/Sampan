/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * PC-side helpers shared by the host tools.
 */
#ifndef SAMPAN_HOST_SERIAL_H
#define SAMPAN_HOST_SERIAL_H

#include <stdint.h>
#include <stdio.h>

/* "-" is stdin/stdout. A tty is set to raw 8N1 at `baud` (non-standard rates
 * work on macOS). Anything else is opened as a plain file. Returns fd or -1. */
int host_open(const char *path, int write, unsigned baud);

/* 16-bit PCM WAV writer; sizes are patched by wav_close. */
struct wav_out {
    FILE *f;
    uint32_t frames;
    uint16_t channels;
};
int wav_open(struct wav_out *w, const char *path, uint32_t rate, uint16_t channels);
int wav_write(struct wav_out *w, const int16_t *pcm, uint32_t frames);
int wav_close(struct wav_out *w);

/* 16-bit PCM WAV reader: positions the file at the start of the samples. */
struct wav_in {
    FILE *f;
    uint32_t rate;
    uint16_t channels;
    uint32_t frames_left;
};
int wav_read_open(struct wav_in *w, const char *path);
/* Returns frames read (0 at the end). */
uint32_t wav_read(struct wav_in *w, int16_t *pcm, uint32_t frames);

#endif
