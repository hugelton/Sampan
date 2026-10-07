/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Minimal PNG writer (RGB, uncompressed deflate) for screen previews.
 */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int b = 0; b < 8; b++)
                k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t h[8];
    be32(h, (uint32_t)len);
    memcpy(h + 4, type, 4);
    uint32_t c = crc32_update(0xFFFFFFFFu, h + 4, 4);
    c = crc32_update(c, data, len) ^ 0xFFFFFFFFu;
    uint8_t t[4];
    be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && fwrite(data, 1, len, f) == len && fwrite(t, 1, 4, f) == 4
               ? 0
               : -1;
}

int png_write_rgb565(const char *path, const uint16_t *px, int w, int h, int scale)
{
    if (scale < 1)
        scale = 1;
    int W = w * scale, H = h * scale;
    size_t raw_len = (size_t)H * (1 + (size_t)W * 3);
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_len), *z = malloc(z_len);
    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }

    uint8_t *r = raw;
    for (int y = 0; y < H; y++) {
        *r++ = 0; /* filter: none */
        for (int x = 0; x < W; x++) {
            uint16_t c = px[(y / scale) * w + x / scale];
            uint8_t R = (uint8_t)((c >> 11) & 31), G = (uint8_t)((c >> 5) & 63), B = (uint8_t)(c & 31);
            *r++ = (uint8_t)(R << 3 | R >> 2);
            *r++ = (uint8_t)(G << 2 | G >> 4);
            *r++ = (uint8_t)(B << 3 | B >> 2);
        }
    }

    uint8_t *o = z;
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *o++ = off + n == raw_len ? 1 : 0;
        *o++ = (uint8_t)n;
        *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n;
        *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[off + i]) % 65521;
            b = (b + a) % 65521;
        }
    }
    be32(o, b << 16 | a);
    o += 4;

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)W);
    be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    FILE *f = fopen(path, "wb");
    int rc = -1;
    if (f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) == 0 &&
        chunk(f, "IDAT", z, (size_t)(o - z)) == 0 && chunk(f, "IEND", NULL, 0) == 0)
        rc = 0;
    if (f && fclose(f) != 0)
        rc = -1;
    free(raw);
    free(z);
    return rc;
}
