/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 */
#include "board.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct board board;

static i2c_master_bus_handle_t new_bus(int port, int sda, int scl)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK)
        return NULL;
    return bus;
}

bool board_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return dev && i2c_master_transmit(dev, b, 2, 50) == ESP_OK;
}

bool board_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return dev && i2c_master_transmit_receive(dev, &reg, 1, val, 1, 50) == ESP_OK;
}

/* Backlight: an RGBW LED driver whose W channel dims the panel. Enable it,
 * run on its internal clock, drive the outputs directly from registers. */
bool board_init(void)
{
    board.sys = new_bus(I2C_NUM_0, PIN_SYS_SDA, PIN_SYS_SCL);
    board.ext = new_bus(I2C_NUM_1, PIN_EXT_SDA, PIN_EXT_SCL);

    gpio_config_t btn = {
        .pin_bit_mask = 1ull << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    if (!board.sys)
        return false;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR_BACKLIGHT,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(board.sys, &dc, &board.backlight) != ESP_OK)
        return false;
    if (!board_reg_write(board.backlight, 0x00, 0x40)) /* chip enable */
        return false;
    vTaskDelay(pdMS_TO_TICKS(1));
    board_reg_write(board.backlight, 0x08, 0x01); /* internal clock */
    board_reg_write(board.backlight, 0x70, 0x00); /* outputs from registers */
    return true;
}

void board_backlight(uint8_t level)
{
    board_reg_write(board.backlight, 0x0E, level); /* W channel PWM */
}

bool board_button(void)
{
    return gpio_get_level(PIN_BUTTON) == 0;
}
