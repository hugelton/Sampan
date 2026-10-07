# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
idf_component_register(
    SRCS main.c board.c es8311.c lcd.c usb.c ble_midi.c sampler.c
    INCLUDE_DIRS . tusb
    REQUIRES sampan_core esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_gpio
             esp_timer usb espressif__tinyusb bt nvs_flash fatfs esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# TinyUSB reads its configuration from tusb_config.h on its own include path.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/tusb)
