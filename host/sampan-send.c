/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * sampan-send: produce a Sampan stream from a sine or a 16-bit WAV, to stdout,
 * a file or a serial port. Stands in for the device when testing receivers.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sampan/proto.h"
#include "sampan/tx.h"
#include "serial.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: sampan-send [options] [out|-]\n"
            "  -c pcm|adpcm  codec (default adpcm)\n"
            "  -i in.wav     send a 16-bit WAV instead of a sine\n"
            "  -f hz         sine frequency (default 440)\n"
            "  -r rate       sine sample rate (default 16000)\n"
            "  -d seconds    sine length (default 2)\n"
            "  -n frames     frames per packet (default 256, max %d)\n"
            "  -w ref.wav    also write the PCM that was sent\n"
            "  -b baud       serial speed (default 921600)\n"
            "  -R            pace in real time\n"
            "  -S seq        first sequence number (default 0)\n",
            SAMPAN_MAX_FRAMES);
    exit(2);
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t codec = SAMPAN_CODEC_ADPCM;
    const char *in = NULL, *ref = NULL;
    double hz = 440, seconds = 2;
    uint32_t rate = 16000;
    unsigned baud = 921600, frames = 256;
    int realtime = 0, opt;
    unsigned first_seq = 0;

    while ((opt = getopt(argc, argv, "c:i:f:r:d:n:w:b:RS:")) != -1) {
        switch (opt) {
        case 'c':
            if (strcmp(optarg, "pcm") == 0)
                codec = SAMPAN_CODEC_PCM16;
            else if (strcmp(optarg, "adpcm") == 0)
                codec = SAMPAN_CODEC_ADPCM;
            else
                usage();
            break;
        case 'i': in = optarg; break;
        case 'f': hz = atof(optarg); break;
        case 'r': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'd': seconds = atof(optarg); break;
        case 'n': frames = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'w': ref = optarg; break;
        case 'b': baud = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'R': realtime = 1; break;
        case 'S': first_seq = (unsigned)strtoul(optarg, NULL, 10); break;
        default: usage();
        }
    }
    if (optind < argc - 1 || frames < 1 || frames > SAMPAN_MAX_FRAMES)
        usage();
    const char *out = optind < argc ? argv[optind] : "-";

    struct wav_in win = {0};
    uint16_t channels = 1;
    uint32_t total;
    if (in) {
        if (wav_read_open(&win, in) < 0) {
            fprintf(stderr, "%s: not a 16-bit mono/stereo WAV\n", in);
            return 1;
        }
        rate = win.rate;
        channels = win.channels;
        total = win.frames_left;
    } else {
        total = (uint32_t)(seconds * rate);
    }

    int fd = host_open(out, 1, baud);
    if (fd < 0) {
        perror(out);
        return 1;
    }
    struct wav_out wref = {0};
    if (ref && wav_open(&wref, ref, rate, channels) < 0) {
        perror(ref);
        return 1;
    }

    static int16_t pcm[SAMPAN_MAX_CHANNELS * SAMPAN_MAX_FRAMES];
    static uint8_t frame[SAMPAN_MAX_FRAME];
    struct sampan_tx tx;
    sampan_tx_init(&tx);
    tx.seq = (uint16_t)first_seq;

    char info[64];
    snprintf(info, sizeof(info), "sampan-send %s", in ? in : "sine");
    size_t n = sampan_tx_packet(&tx, SAMPAN_TYPE_INFO, (const uint8_t *)info,
                                strlen(info), frame, sizeof(frame));
    if (write_all(fd, frame, n) < 0)
        goto io_error;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t sent = 0;
    while (sent < total) {
        uint32_t k = total - sent < frames ? total - sent : frames;
        if (in) {
            k = wav_read(&win, pcm, k);
            if (k == 0)
                break;
        } else {
            for (uint32_t i = 0; i < k; i++)
                pcm[i] = (int16_t)lrint(12000 * sin(2 * M_PI * hz * (double)(sent + i) / rate));
        }
        n = sampan_tx_audio(&tx, codec, (uint8_t)channels, rate, pcm, (uint16_t)k, frame,
                            sizeof(frame));
        if (n == 0) {
            fprintf(stderr, "encode failed (rate %u?)\n", (unsigned)rate);
            return 1;
        }
        if (write_all(fd, frame, n) < 0)
            goto io_error;
        if (ref)
            wav_write(&wref, pcm, k);
        sent += k;

        if (realtime) {
            double due = (double)sent / rate;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double el = (double)(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
            if (due > el) {
                struct timespec d = {(time_t)(due - el),
                                     (long)((due - el - (time_t)(due - el)) * 1e9)};
                nanosleep(&d, NULL);
            }
        }
    }
    if (ref && wav_close(&wref) < 0) {
        perror(ref);
        return 1;
    }
    if (fd > 2)
        close(fd);
    return 0;

io_error:
    perror(out);
    return 1;
}
