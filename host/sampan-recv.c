/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * sampan-recv: read a Sampan stream from a serial port, a file or stdin and
 * write the audio to a WAV file. Lost packets become silence so timing holds.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sampan/adpcm.h"
#include "sampan/proto.h"
#include "serial.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: sampan-recv [-b baud] [-o out.wav] [-t seconds] [-q] <port|file|->\n"
            "  -b  serial speed (default 921600)\n"
            "  -o  output WAV (default sampan.wav)\n"
            "  -t  stop after this many seconds of audio\n"
            "  -q  no progress line\n");
    exit(2);
}

struct state {
    struct wav_out wav;
    const char *out;
    int have_fmt;
    uint8_t channels;
    uint32_t rate;
    uint32_t limit_frames;
    struct sampan_seq seq;
    uint32_t format_changes;
    uint32_t resyncs;
};

static int16_t pcm[SAMPAN_MAX_CHANNELS * SAMPAN_MAX_FRAMES];

static int write_silence(struct state *s, uint32_t frames)
{
    memset(pcm, 0, sizeof(pcm));
    while (frames) {
        uint32_t n = frames > SAMPAN_MAX_FRAMES ? SAMPAN_MAX_FRAMES : frames;
        if (wav_write(&s->wav, pcm, n) < 0)
            return -1;
        frames -= n;
    }
    return 0;
}

/* gap: packets missed just before this one (any type; counted as audio). */
static int on_audio(struct state *s, const struct sampan_packet *p, uint32_t gap)
{
    struct sampan_audio a;
    if (sampan_audio_parse(p->body, p->body_len, &a) != SAMPAN_OK)
        return 0;

    if (!s->have_fmt) {
        if (wav_open(&s->wav, s->out, a.rate, a.channels) < 0) {
            perror(s->out);
            return -1;
        }
        s->have_fmt = 1;
        s->channels = a.channels;
        s->rate = a.rate;
        fprintf(stderr, "stream: %u Hz, %u ch, %s\n", (unsigned)a.rate,
                (unsigned)a.channels, a.codec == SAMPAN_CODEC_ADPCM ? "ADPCM" : "PCM16");
    } else if (a.rate != s->rate || a.channels != s->channels) {
        s->format_changes++;
        return 0; /* one WAV holds one format */
    }

    /* Short gaps become silence so timing holds. A long one means we joined
     * late (data the sender had queued before we opened the port) or the
     * sender restarted: carry on without padding. */
    uint64_t fill = (uint64_t)gap * a.frames;
    if (fill > s->rate / 2) {
        s->seq.lost -= gap;
        s->resyncs++;
        fill = 0;
    }
    if (fill && write_silence(s, (uint32_t)fill) < 0)
        return -1;

    if (a.codec == SAMPAN_CODEC_PCM16) {
        for (size_t i = 0; i < (size_t)a.frames * a.channels; i++)
            pcm[i] = (int16_t)(a.data[2 * i] | a.data[2 * i + 1] << 8);
    } else {
        size_t per_ch = (a.frames + 1u) / 2;
        for (uint8_t c = 0; c < a.channels; c++) {
            struct sampan_adpcm st = {a.adpcm_pred[c], a.adpcm_index[c]};
            sampan_adpcm_decode(&st, a.data + c * per_ch, a.frames, pcm + c, a.channels);
        }
    }
    return wav_write(&s->wav, pcm, a.frames);
}

int main(int argc, char **argv)
{
    unsigned baud = 921600;
    double seconds = 0;
    int quiet = 0, opt;
    struct state s = {.out = "sampan.wav"};

    while ((opt = getopt(argc, argv, "b:o:t:q")) != -1) {
        switch (opt) {
        case 'b': baud = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'o': s.out = optarg; break;
        case 't': seconds = atof(optarg); break;
        case 'q': quiet = 1; break;
        default: usage();
        }
    }
    if (optind != argc - 1)
        usage();

    int fd = host_open(argv[optind], 0, baud);
    if (fd < 0) {
        perror(argv[optind]);
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal; /* no SA_RESTART: read() returns EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    static struct sampan_rx rx;
    sampan_rx_init(&rx);
    uint8_t buf[4096];
    time_t last_report = 0;
    uint32_t pending_gap = 0;
    int rc = 0;

    while (!stop) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n && !stop; i++) {
            struct sampan_packet p;
            if (sampan_rx_push(&rx, buf[i], &p) != 1)
                continue;
            /* Sequence numbers run across all packet types. */
            pending_gap += sampan_seq_update(&s.seq, p.seq);
            if (p.type == SAMPAN_TYPE_AUDIO) {
                uint32_t gap = pending_gap;
                pending_gap = 0;
                if (on_audio(&s, &p, gap) < 0) {
                    rc = 1;
                    stop = 1;
                }
            } else if (p.type == SAMPAN_TYPE_INFO) {
                fprintf(stderr, "info: %.*s\n", (int)p.body_len, (const char *)p.body);
            }
            if (seconds > 0 && s.have_fmt && s.wav.frames >= seconds * s.rate)
                stop = 1;
        }
        time_t now = time(NULL);
        if (!quiet && s.have_fmt && now != last_report) {
            last_report = now;
            fprintf(stderr, "\r%.1f s  packets %u  lost %u  crc %u  cobs %u   ",
                    (double)s.wav.frames / s.rate, (unsigned)rx.packets,
                    (unsigned)s.seq.lost, (unsigned)rx.crc_errors,
                    (unsigned)rx.cobs_errors);
        }
    }

    if (wav_close(&s.wav) < 0) {
        perror(s.out);
        rc = 1;
    }
    fprintf(stderr,
            "%sdone: %.2f s, packets %u, lost %u, crc errors %u, cobs errors %u, "
            "overflows %u, format changes ignored %u, resyncs %u\n",
            quiet ? "" : "\n", s.rate ? (double)s.wav.frames / s.rate : 0.0,
            (unsigned)rx.packets, (unsigned)s.seq.lost, (unsigned)rx.crc_errors,
            (unsigned)rx.cobs_errors, (unsigned)rx.overflows, (unsigned)s.format_changes, (unsigned)s.resyncs);
    if (!s.have_fmt) {
        fprintf(stderr, "no audio received\n");
        rc = 1;
    }
    if (fd > 2)
        close(fd);
    return rc;
}
