/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

static speed_t std_speed(unsigned baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
    default: return 0;
    }
}

static int setup_tty(int fd, unsigned baud)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CRTSCTS);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    speed_t sp = std_speed(baud);
#ifdef __APPLE__
    if (sp == 0) {
        cfsetspeed(&t, B115200);
        if (tcsetattr(fd, TCSANOW, &t) < 0)
            return -1;
        speed_t custom = baud;
        return ioctl(fd, IOSSIOSPEED, &custom);
    }
#else
    if (sp == 0) {
        errno = EINVAL;
        return -1;
    }
#endif
    cfsetspeed(&t, sp);
    return tcsetattr(fd, TCSANOW, &t);
}

int host_open(const char *path, int write, unsigned baud)
{
    if (strcmp(path, "-") == 0)
        return write ? 1 : 0;
    int flags = write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
    int fd = open(path, flags | O_NOCTTY, 0644);
    if (fd < 0)
        return -1;
    if (isatty(fd) && setup_tty(fd, baud) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, (uint16_t)v);
    le16(p + 2, (uint16_t)(v >> 16));
}

static void wav_header(uint8_t h[44], uint32_t rate, uint16_t ch, uint32_t frames)
{
    uint32_t data = frames * ch * 2u;
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);
    le16(h + 22, ch);
    le32(h + 24, rate);
    le32(h + 28, rate * ch * 2u);
    le16(h + 32, (uint16_t)(ch * 2));
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}

int wav_open(struct wav_out *w, const char *path, uint32_t rate, uint16_t channels)
{
    uint8_t h[44];
    w->f = fopen(path, "wb");
    if (!w->f)
        return -1;
    w->frames = 0;
    w->channels = channels;
    wav_header(h, rate, channels, 0);
    return fwrite(h, 1, 44, w->f) == 44 ? 0 : -1;
}

int wav_write(struct wav_out *w, const int16_t *pcm, uint32_t frames)
{
    uint8_t buf[512];
    size_t total = (size_t)frames * w->channels, done = 0;
    while (done < total) {
        size_t n = total - done;
        if (n > sizeof(buf) / 2)
            n = sizeof(buf) / 2;
        for (size_t i = 0; i < n; i++)
            le16(buf + 2 * i, (uint16_t)pcm[done + i]);
        if (fwrite(buf, 2, n, w->f) != n)
            return -1;
        done += n;
    }
    w->frames += frames;
    return 0;
}

int wav_close(struct wav_out *w)
{
    if (!w->f)
        return 0;
    uint8_t v[4];
    uint32_t data = w->frames * w->channels * 2u;
    int r = 0;
    le32(v, 36 + data);
    if (fseek(w->f, 4, SEEK_SET) != 0 || fwrite(v, 1, 4, w->f) != 4)
        r = -1;
    le32(v, data);
    if (fseek(w->f, 40, SEEK_SET) != 0 || fwrite(v, 1, 4, w->f) != 4)
        r = -1;
    if (fclose(w->f) != 0)
        r = -1;
    w->f = NULL;
    return r;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int wav_read_open(struct wav_in *w, const char *path)
{
    uint8_t h[12], ck[8], fmt[16];
    int have_fmt = 0;
    w->f = fopen(path, "rb");
    if (!w->f)
        return -1;
    if (fread(h, 1, 12, w->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        goto bad;
    while (fread(ck, 1, 8, w->f) == 8) {
        uint32_t size = rd32(ck + 4);
        if (memcmp(ck, "fmt ", 4) == 0) {
            if (size < 16 || fread(fmt, 1, 16, w->f) != 16)
                goto bad;
            if (fseek(w->f, (long)(size - 16 + (size & 1)), SEEK_CUR) != 0)
                goto bad;
            uint16_t format = (uint16_t)(fmt[0] | fmt[1] << 8);
            w->channels = (uint16_t)(fmt[2] | fmt[3] << 8);
            w->rate = rd32(fmt + 4);
            uint16_t bits = (uint16_t)(fmt[14] | fmt[15] << 8);
            if ((format != 1 && format != 0xFFFE) || bits != 16 || w->channels < 1 ||
                w->channels > 2)
                goto bad;
            have_fmt = 1;
        } else if (memcmp(ck, "data", 4) == 0) {
            if (!have_fmt)
                goto bad;
            w->frames_left = size / (2u * w->channels);
            return 0;
        } else if (fseek(w->f, (long)(size + (size & 1)), SEEK_CUR) != 0) {
            goto bad;
        }
    }
bad:
    fclose(w->f);
    w->f = NULL;
    return -1;
}

uint32_t wav_read(struct wav_in *w, int16_t *pcm, uint32_t frames)
{
    uint8_t buf[4];
    uint32_t n = 0;
    if (frames > w->frames_left)
        frames = w->frames_left;
    for (; n < frames; n++) {
        if (fread(buf, 2, w->channels, w->f) != w->channels)
            break;
        for (uint16_t c = 0; c < w->channels; c++)
            pcm[n * w->channels + c] = (int16_t)(buf[2 * c] | buf[2 * c + 1] << 8);
    }
    w->frames_left -= n;
    return n;
}
