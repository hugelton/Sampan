/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * sampan-scope: run the waveform display on a WAV file or a test signal and
 * save what the screen would show, as PNG files and/or text.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "png.h"
#include "sampan/scope.h"
#include "serial.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: sampan-scope [options]\n"
            "  -i in.wav     input (16-bit; first channel); default is a test signal\n"
            "  -f hz         test signal frequency (default 220)\n"
            "  -A amp        test signal peak, 0..1.2 (default 0.5; above 1 clips)\n"
            "  -r rate       test signal sample rate (default 48000)\n"
            "  -d seconds    test signal length (default 1)\n"
            "  -W -H         screen size (default 128 x 128)\n"
            "  -w ms         time across the screen (default 20)\n"
            "  -p palette    0 green, 1 amber, 2 cyan, 3 red, 4 mono (default 4)\n"
            "  -z gain       fixed gain instead of auto scale (1 = full scale)\n"
            "  -F            free-run (no trigger)\n"
            "  -o prefix     write prefix-NNN.png for each displayed frame\n"
            "  -n count      at most this many PNG files (default 8)\n"
            "  -s scale      PNG pixel scale (default 4)\n"
            "  -t            print the last frame as text\n");
    exit(2);
}

/* Test signal: a tone with one harmonic and a slow detune, so the trigger
 * has something to do. */
static int16_t test_sample(uint32_t i, double hz, double amp, uint32_t rate)
{
    double t = (double)i / rate;
    double v = amp * (0.8 * sin(2 * M_PI * hz * t) + 0.2 * sin(2 * M_PI * hz * 3.01 * t + 0.5));
    double s = v * 32768.0;
    if (s > 32767)
        s = 32767;
    if (s < -32768)
        s = -32768;
    return (int16_t)lrint(s);
}

static void print_text(const uint16_t *fb, int w, int h, uint16_t bg, uint16_t axis)
{
    int cols = w < 64 ? w : 64, rows = h / (w / cols) / 2;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int x0 = c * w / cols, x1 = (c + 1) * w / cols, y0 = r * h / rows, y1 = (r + 1) * h / rows;
            char ch = ' ';
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint16_t p = fb[y * w + x];
                    if (p != bg && p != axis)
                        ch = '#';
                    else if (p == axis && ch == ' ')
                        ch = '-';
                }
            putchar(ch);
        }
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    const char *in = NULL, *prefix = NULL;
    double hz = 220, amp = 0.5, seconds = 1, ms = 20, zoom = 0;
    uint32_t rate = 48000;
    int W = 128, H = 128, pal = SAMPAN_PAL_DEFAULT, count = 8, scale = 4, text = 0, freerun = 0, opt;

    while ((opt = getopt(argc, argv, "i:f:A:r:d:W:H:w:p:z:Fo:n:s:t")) != -1) {
        switch (opt) {
        case 'i': in = optarg; break;
        case 'f': hz = atof(optarg); break;
        case 'A': amp = atof(optarg); break;
        case 'r': rate = (uint32_t)atoi(optarg); break;
        case 'd': seconds = atof(optarg); break;
        case 'W': W = atoi(optarg); break;
        case 'H': H = atoi(optarg); break;
        case 'w': ms = atof(optarg); break;
        case 'p': pal = atoi(optarg); break;
        case 'z': zoom = atof(optarg); break;
        case 'F': freerun = 1; break;
        case 'o': prefix = optarg; break;
        case 'n': count = atoi(optarg); break;
        case 's': scale = atoi(optarg); break;
        case 't': text = 1; break;
        default: usage();
        }
    }
    if (W < 1 || W > SAMPAN_SCOPE_MAX_W || H < 8 || H > 1024 || optind != argc)
        usage();

    struct wav_in win;
    uint32_t total;
    if (in) {
        if (wav_read_open(&win, in) < 0) {
            fprintf(stderr, "%s: not a 16-bit WAV\n", in);
            return 1;
        }
        rate = win.rate;
        total = win.frames_left;
    } else {
        total = (uint32_t)(seconds * rate);
    }

    static struct sampan_scope sc;
    struct sampan_scope_cfg cfg;
    sampan_scope_cfg_default(&cfg, (uint16_t)W, rate);
    cfg.window = (uint32_t)(ms * rate / 1000);
    cfg.trigger = !freerun;
    sampan_scope_init(&sc, &cfg);

    struct sampan_scope_style st;
    sampan_scope_style_default(&st, (uint8_t)pal);
    if (zoom > 0) {
        st.auto_scale = false;
        st.zoom_q8 = (uint16_t)(zoom * 256);
    }

    uint16_t *fb = calloc((size_t)W * H, sizeof(uint16_t));
    static struct sampan_scope_spans sp;
    /* The display reads at 30 frames per second of audio time. */
    uint32_t per_frame = rate / 30, done = 0;
    int shown = 0, written = 0, fresh_frames = 0, triggered = 0;
    static int16_t buf[2 * 48000];
    while (done < total) {
        uint32_t n = total - done < per_frame ? total - done : per_frame;
        if (in) {
            n = wav_read(&win, buf, n);
            if (n == 0)
                break;
            sampan_scope_push(&sc, buf, n, win.channels);
        } else {
            for (uint32_t i = 0; i < n; i++)
                buf[i] = test_sample(done + i, hz, amp, rate);
            sampan_scope_push(&sc, buf, n, 1);
        }
        done += n;

        bool fresh;
        const struct sampan_scope_frame *f = sampan_scope_latest(&sc, &fresh);
        if (!f)
            continue;
        shown++;
        fresh_frames += fresh;
        triggered += fresh && f->triggered;
        sampan_scope_spans(f, (uint16_t)H, &st, &sp);
        sampan_scope_render(&sp, &st, 0, (uint16_t)H, fb);
        if (prefix && written < count) {
            char path[1024];
            snprintf(path, sizeof(path), "%s-%03d.png", prefix, written);
            if (png_write_rgb565(path, fb, W, H, scale) < 0) {
                perror(path);
                return 1;
            }
            written++;
        }
    }
    if (text && shown)
        print_text(fb, W, H, st.bg, st.axis);
    fprintf(stderr, "displayed %d, new %d (triggered %d), scope frames made %u, png %d\n",
            shown, fresh_frames, triggered, (unsigned)sc.seq, written);
    free(fb);
    return shown ? 0 : 1;
}
