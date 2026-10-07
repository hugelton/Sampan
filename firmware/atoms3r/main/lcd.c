/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Panel bring-up. The controller-specific register lists below are the
 * values the LovyanGFX / M5GFX panel drivers use (FreeBSD and MIT licences,
 * see LICENSING.md); the rest is standard display command set.
 */
#include "lcd.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DELAY 0x80 /* flag in the count byte: a delay in ms follows the args */

static spi_device_handle_t spi;
static enum lcd_kind kind;
static uint16_t off_x, off_y;

static const uint8_t init_gc9107[] = {
    0xFE, DELAY, 5,
    0xEF, DELAY, 5,
    0xB0, 1, 0xC0,
    0xB2, 1, 0x2F,
    0xB3, 1, 0x03,
    0xB6, 1, 0x19,
    0xB7, 1, 0x01,
    0xAC, 1, 0xCB,
    0xAB, 1, 0x0E,
    0xB4, 1, 0x04,
    0xA8, 1, 0x19,
    0xB8, 1, 0x08,
    0xE8, 1, 0x24,
    0xE9, 1, 0x48,
    0xEA, 1, 0x22,
    0xC6, 1, 0x30,
    0xC7, 1, 0x18,
    0xF0, 14, 0x01, 0x2b, 0x23, 0x3c, 0xb7, 0x12, 0x17, 0x60, 0x00, 0x06, 0x0c, 0x17, 0x12, 0x1f,
    0xF1, 14, 0x05, 0x2e, 0x2d, 0x44, 0xd6, 0x15, 0x17, 0xa0, 0x02, 0x0d, 0x0d, 0x1a, 0x18, 0x1f,
    0x11, DELAY, 120,
    0x3A, 1, 0x55, /* 16-bit pixels */
    0x36, 1, 0x08, /* BGR */
    0x20, 0,       /* no inversion */
    0x29, 0,
    0x00, 0xFF,
};

static const uint8_t init_st7735s[] = {
    0x01, DELAY, 150,
    0x11, DELAY, 255,
    0xB1, 3, 0x01, 0x2C, 0x2D,
    0xB2, 3, 0x01, 0x2C, 0x2D,
    0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,
    0xB4, 1, 0x07,
    0xC0, 3, 0xA2, 0x02, 0x84,
    0xC1, 1, 0xC5,
    0xC2, 2, 0x0A, 0x00,
    0xC3, 2, 0x8A, 0x2A,
    0xC4, 2, 0x8A, 0xEE,
    0xC5, 1, 0x0E,
    0xE0, 16, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
    0xE1, 16, 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
    0x3A, 1, 0x05,
    0x36, 1, 0xC8, /* rotated 180, BGR */
    0x21, 0,       /* inverted panel */
    0x13, DELAY, 10,
    0x29, DELAY, 100,
    0x00, 0xFF,
};

static void send(bool data, const uint8_t *b, size_t n)
{
    if (!n)
        return;
    gpio_set_level(PIN_LCD_DC, data);
    spi_transaction_t t = {.length = n * 8, .tx_buffer = b};
    spi_device_polling_transmit(spi, &t);
}

static void cmd(uint8_t c, const uint8_t *args, size_t n)
{
    send(false, &c, 1);
    send(true, args, n);
}

static void run(const uint8_t *p)
{
    for (;;) {
        uint8_t c = *p++, n = *p++;
        if (c == 0x00 && n == 0xFF)
            return;
        cmd(c, p, n & 0x7F);
        p += n & 0x7F;
        if (n & DELAY)
            vTaskDelay(pdMS_TO_TICKS(*p++ == 255 ? 500 : p[-1]));
    }
}

static void add_device(int hz)
{
    spi_device_interface_config_t dev = {
        .clock_speed_hz = hz,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 2,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX,
    };
    spi_bus_add_device(SPI3_HOST, &dev, &spi);
}

/* RDDID with one dummy clock, read on the shared data line. */
static uint32_t read_id(void)
{
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_ext_t t = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA,
            .cmd = 0x04,
            .rxlength = 32,
        },
        .command_bits = 8,
        .dummy_bits = 1,
    };
    if (spi_device_polling_transmit(spi, &t.base) != ESP_OK)
        return 0;
    const uint8_t *r = t.base.rx_data;
    return (uint32_t)r[0] | (uint32_t)r[1] << 8 | (uint32_t)r[2] << 16 | (uint32_t)r[3] << 24;
}

static enum lcd_kind classify(uint32_t id)
{
    if ((id & 0xFFFFFF) == 0x079100)
        return LCD_GC9107;
    if ((id & 0xFFFF) == 0x7683 || (id & 0xFFFF) == 0x897C)
        return LCD_ST7735S;
    return LCD_UNKNOWN;
}

enum lcd_kind lcd_init(uint32_t *id_out)
{
    gpio_config_t out = {
        .pin_bit_mask = 1ull << PIN_LCD_DC | 1ull << PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 32 * 2,
    };
    spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);

    /* Some batches only answer the ID read slowly. */
    add_device(8000000);
    uint32_t id = read_id();
    kind = classify(id);
    if (kind == LCD_UNKNOWN) {
        spi_bus_remove_device(spi);
        add_device(100000);
        id = read_id();
        kind = classify(id);
    }
    spi_bus_remove_device(spi);
    add_device(40000000);
    if (id_out)
        *id_out = id;

    if (kind == LCD_ST7735S) {
        run(init_st7735s);
        off_x = 2; /* 132 x 132 memory, panel rotated 180 */
        off_y = 3;
    } else {
        run(init_gc9107);
        off_x = 0; /* 128 x 160 memory */
        off_y = 32;
    }
    return kind;
}

void lcd_rows(uint16_t y, uint16_t rows, const uint16_t *px)
{
    uint16_t x0 = off_x, x1 = off_x + LCD_W - 1, y0 = off_y + y, y1 = off_y + y + rows - 1;
    uint8_t ca[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t ra[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    uint8_t c = 0x2C;
    send(false, &c, 1);
    send(true, (const uint8_t *)px, (size_t)LCD_W * rows * 2);
}
