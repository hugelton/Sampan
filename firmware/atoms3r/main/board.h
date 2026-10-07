/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * AtomS3R with the Echo Base: pins and the two I2C buses.
 */
#ifndef SAMPAN_BOARD_H
#define SAMPAN_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

/* LCD on SPI3, 3-wire (MOSI also reads). */
#define PIN_LCD_MOSI 21
#define PIN_LCD_SCLK 15
#define PIN_LCD_CS 14
#define PIN_LCD_DC 42
#define PIN_LCD_RST 48

/* Internal I2C: backlight LED driver (0x30), IMU. */
#define PIN_SYS_SDA 45
#define PIN_SYS_SCL 0
#define ADDR_BACKLIGHT 0x30

/* Echo Base: codec on its own I2C, I2S without MCLK. */
#define PIN_EXT_SDA 38
#define PIN_EXT_SCL 39
#define PIN_I2S_BCLK 8
#define PIN_I2S_WS 6
#define PIN_I2S_DIN 7  /* codec ADC -> ESP */
#define PIN_I2S_DOUT 5 /* ESP -> codec DAC */

#define PIN_BUTTON 41 /* low when pressed */

struct board {
    i2c_master_bus_handle_t sys, ext;
    i2c_master_dev_handle_t backlight;
};

extern struct board board;

/* Buses, backlight and button. Returns false if the backlight driver didn't answer. */
bool board_init(void);
void board_backlight(uint8_t level);
bool board_button(void);

/* Register write / read on a device handle. */
bool board_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
bool board_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val);

#endif
