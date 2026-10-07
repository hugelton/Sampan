/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#ifndef SAMPAN_HOST_PNG_H
#define SAMPAN_HOST_PNG_H

#include <stdint.h>

/* Write w x h RGB565 pixels (native byte order) as a PNG, each pixel blown
 * up to scale x scale. Returns 0 or -1. */
int png_write_rgb565(const char *path, const uint16_t *px, int w, int h, int scale);

#endif
