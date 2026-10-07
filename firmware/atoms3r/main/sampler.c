/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Takes are /data/T0001.WAV, T0002.WAV, ... (8.3 names, 48 kHz mono 16-bit).
 * Recording writes into one PSRAM buffer; on stop a worker task trims the
 * silence, evens out the level, fades the ends, swaps the result into the
 * play buffer and writes the file.
 */
#include "sampler.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sampan/sample.h"

#define MAX_FRAMES (SAMPLER_RATE * SAMPLER_MAX_S)
#define BASE "/data"

static int16_t *rec_buf, *play_buf;
static volatile size_t rec_len, play_len;
static volatile bool recording, saving, ready, psram;
static volatile uint32_t takes, next_take = 1;
static volatile int last_error;
static volatile float master = 0.25f; /* quiet by default */
static struct sampan_voice voice;
static SemaphoreHandle_t voice_lock, work;

/* ---- files ------------------------------------------------------------------ */

static void scan(void)
{
    DIR *d = opendir(BASE);
    if (!d)
        return;
    struct dirent *e;
    uint32_t n = 0, hi = 0;
    while ((e = readdir(d)) != NULL) {
        unsigned k;
        if (sscanf(e->d_name, "T%4u.WAV", &k) == 1 || sscanf(e->d_name, "t%4u.wav", &k) == 1) {
            n++;
            if (k > hi)
                hi = k;
        }
    }
    closedir(d);
    takes = n;
    next_take = hi + 1;
}

static bool load(uint32_t k)
{
    char path[32];
    snprintf(path, sizeof(path), BASE "/T%04u.WAV", (unsigned)k);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t h[44];
    size_t n = 0;
    if (fread(h, 1, 44, f) == 44)
        n = fread(play_buf, 2, MAX_FRAMES, f);
    fclose(f);
    play_len = n;
    return n > 0;
}

static int save(const int16_t *x, size_t n)
{
    char path[32];
    snprintf(path, sizeof(path), BASE "/T%04u.WAV", (unsigned)next_take);
    FILE *f = fopen(path, "wb");
    if (!f)
        return ESP_FAIL;
    uint8_t h[44];
    sampan_wav_header(h, SAMPLER_RATE, 1, (uint32_t)n);
    bool ok = fwrite(h, 1, 44, f) == 44 && fwrite(x, 2, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        return ESP_FAIL;
    next_take++;
    takes++;
    return ESP_OK;
}

/* ---- worker: tidy and save a finished take ---------------------------------- */

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(work, portMAX_DELAY);
        size_t start, n = sampan_sample_trim(rec_buf, rec_len, 300, SAMPLER_RATE / 100,
                                             SAMPLER_RATE / 20, &start);
        if (n > SAMPLER_RATE / 50) { /* keep takes longer than 20 ms */
            int16_t *x = rec_buf + start;
            sampan_sample_normalize(x, n, 23000, 8.0f); /* about -3 dBFS, at most +18 dB */
            sampan_sample_fade(x, n, SAMPLER_RATE / 1000, SAMPLER_RATE / 100);

            xSemaphoreTake(voice_lock, portMAX_DELAY);
            voice.on = false;
            memcpy(play_buf, x, n * 2);
            play_len = n;
            xSemaphoreGive(voice_lock);

            last_error = save(play_buf, n);
        }
        saving = false;
    }
}

/* ---- API -------------------------------------------------------------------- */

bool sampler_init(void)
{
    voice_lock = xSemaphoreCreateMutex();
    work = xSemaphoreCreateBinary();

    size_t bytes = MAX_FRAMES * sizeof(int16_t);
    rec_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    play_buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    psram = rec_buf && play_buf;
    if (!psram)
        return false;

    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = true,
        .max_files = 2,
        .allocation_unit_size = 4096,
    };
    static wl_handle_t wl;
    if (esp_vfs_fat_spiflash_mount_rw_wl(BASE, "storage", &mc, &wl) != ESP_OK)
        return false;
    scan();
    if (next_take > 1)
        load(next_take - 1);
    xTaskCreatePinnedToCore(worker, "sampler", 4096, NULL, 4, NULL, 0);
    ready = true;
    return true;
}

void sampler_rec_start(void)
{
    if (!ready || saving)
        return;
    rec_len = 0;
    recording = true;
}

void sampler_rec_stop(void)
{
    if (!recording)
        return;
    recording = false;
    saving = true;
    xSemaphoreGive(work);
}

void sampler_input(const int16_t *x, size_t n)
{
    if (!recording)
        return;
    size_t room = MAX_FRAMES - rec_len;
    if (n > room)
        n = room;
    memcpy(rec_buf + rec_len, x, n * 2);
    rec_len += n;
    if (rec_len >= MAX_FRAMES)
        sampler_rec_stop(); /* full: stop by itself */
}

void sampler_play(float semitones, float velocity)
{
    if (!ready || !play_len)
        return;
    xSemaphoreTake(voice_lock, portMAX_DELAY);
    sampan_voice_start(&voice, play_buf, play_len, semitones, velocity * master);
    xSemaphoreGive(voice_lock);
}

void sampler_stop(void)
{
    voice.on = false;
}

void sampler_set_volume(float v)
{
    master = v < 0 ? 0 : v > 1 ? 1 : v;
}

void sampler_render(int16_t *out, size_t n)
{
    /* Never wait in the audio task: skip a block if the worker holds it. */
    if (!voice.on || xSemaphoreTake(voice_lock, 0) != pdTRUE)
        return;
    sampan_voice_mix(&voice, out, n);
    xSemaphoreGive(voice_lock);
}

void sampler_status(struct sampler_status *s)
{
    s->ready = ready;
    s->recording = recording;
    s->playing = voice.on;
    s->saving = saving;
    s->takes = takes;
    s->last_s = (float)play_len / SAMPLER_RATE;
    s->rec_s = (float)rec_len / SAMPLER_RATE;
    s->last_error = last_error;
    s->psram = psram;
    s->free_kb = 0;
    uint64_t total = 0, free_b = 0;
    if (ready && esp_vfs_fat_info(BASE, &total, &free_b) == ESP_OK)
        s->free_kb = (uint32_t)(free_b / 1024);
}
