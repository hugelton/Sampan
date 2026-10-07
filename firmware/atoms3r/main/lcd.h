/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * 128 x 128 panel. Two controllers have shipped on this board; the one
 * present is told apart by its ID (RDDID) at start-up.
 */
#ifndef SAMPAN_LCD_H
#define SAMPAN_LCD_H

#include <stdint.h>

#define LCD_W 128
#define LCD_H 128

enum lcd_kind { LCD_UNKNOWN, LCD_GC9107, LCD_ST7735S };

/* Returns the controller found (LCD_UNKNOWN: initialised as GC9107 anyway). */
enum lcd_kind lcd_init(uint32_t *id_out);

/* Send rows [y, y + rows) of full width; pixels already byte-swapped. */
void lcd_rows(uint16_t y, uint16_t rows, const uint16_t *px);

#endif
