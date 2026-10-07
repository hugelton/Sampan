/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Register values follow the codec datasheet; the clocking for this board
 * (master clock taken from BCLK, no MCLK line) matches what the vendor's
 * MIT-licensed library uses for the same hardware.
 */
#include "es8311.h"

#include "board.h"

static i2c_master_dev_handle_t dev, ioexp;

#define IOEXP_ADDR 0x43 /* amplifier enable on the base */

bool es8311_open(i2c_master_bus_handle_t bus, uint16_t *chip_id)
{
    if (!bus || i2c_master_probe(bus, ES8311_ADDR, 50) != ESP_OK)
        return false;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_ADDR,
        .scl_speed_hz = 400000,
    };
    if (!dev && i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK)
        return false;
    uint8_t hi = 0, lo = 0;
    if (!board_reg_read(dev, 0xFD, &hi) || !board_reg_read(dev, 0xFE, &lo))
        return false;
    if (chip_id)
        *chip_id = (uint16_t)(hi << 8 | lo);
    return true;
}

bool es8311_mic_on(uint8_t pga, uint8_t adc_vol)
{
    if (pga > 10)
        pga = 10;
    const uint8_t seq[][2] = {
        {0x00, 0x80},              /* reset off, power on */
        {0x01, 0xBA},              /* MCLK from BCLK; ADC clocks on */
        {0x02, 0x18},              /* pre-multiply x8: 32 fs BCLK -> 256 fs */
        {0x0D, 0x01},              /* analog power up */
        {0x0E, 0x02},              /* PGA and ADC modulator on */
        {0x14, (uint8_t)(0x10 | pga)}, /* differential mic 1, PGA gain */
        {0x17, adc_vol},           /* ADC volume */
        {0x1C, 0x6A},              /* EQ bypass, digital DC offset cancel */
    };
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        if (!board_reg_write(dev, seq[i][0], seq[i][1]))
            return false;
    return true;
}

bool es8311_speaker_on(uint8_t dac_vol)
{
    const uint8_t seq[][2] = {
        {0x01, 0xBF}, /* ADC and DAC clocks on */
        {0x12, 0x00}, /* DAC power up */
        {0x13, 0x10}, /* output driver on */
        {0x32, dac_vol},
        {0x37, 0x08}, /* DAC EQ bypass */
    };
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        if (!board_reg_write(dev, seq[i][0], seq[i][1]))
            return false;

    /* Expander: all pins push-pull outputs, high (amplifier on). */
    if (!ioexp) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = IOEXP_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(board.ext, &dc, &ioexp) != ESP_OK)
            return false;
    }
    return board_reg_write(ioexp, 0x03, 0xFF) && board_reg_write(ioexp, 0x07, 0x00) &&
           board_reg_write(ioexp, 0x0B, 0x00) && board_reg_write(ioexp, 0x05, 0xFF);
}
