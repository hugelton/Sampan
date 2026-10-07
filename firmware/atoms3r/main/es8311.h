/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Audio codec on the Echo Base: microphone in, speaker out (through the
 * base's amplifier, switched by an I/O expander).
 */
#ifndef SAMPAN_ES8311_H
#define SAMPAN_ES8311_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#define ES8311_ADDR 0x18

/* Probe and read the chip ID (0xFD/0xFE). Returns false if absent. */
bool es8311_open(i2c_master_bus_handle_t bus, uint16_t *chip_id);

/* Microphone on: clock from BCLK (needs I2S running at 32 x fs), analog PGA
 * gain 0..10 (3 dB steps), digital ADC volume 0xBF = 0 dB. */
bool es8311_mic_on(uint8_t pga, uint8_t adc_vol);

/* Speaker path on as well (call after es8311_mic_on): DAC volume 0xBF = 0 dB,
 * and the amplifier enabled. */
bool es8311_speaker_on(uint8_t dac_vol);

#endif
