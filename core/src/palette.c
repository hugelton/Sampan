/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "sampan/palette.h"

#define C SAMPAN_RGB565

const struct sampan_palette sampan_palettes[SAMPAN_PAL_COUNT] = {
    {"GREEN", {C(0, 40, 12), C(0, 84, 30), C(16, 140, 54), C(56, 200, 92), C(120, 255, 146)}},
    {"AMBER", {C(60, 26, 0), C(110, 50, 0), C(170, 82, 0), C(225, 120, 8), C(255, 166, 40)}},
    {"CYAN", {C(0, 30, 50), C(0, 62, 96), C(16, 112, 160), C(56, 172, 222), C(140, 222, 255)}},
    {"RED", {C(52, 8, 8), C(100, 18, 14), C(170, 36, 26), C(226, 64, 48), C(255, 112, 92)}},
    {"MONO", {C(40, 40, 40), C(80, 80, 80), C(130, 130, 130), C(186, 186, 186), C(226, 226, 226)}},
};
