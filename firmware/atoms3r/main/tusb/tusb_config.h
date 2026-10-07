/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * TinyUSB configuration: one composite device with
 *   UAC2 microphone (48 kHz, 16-bit, mono) + CDC (Sampan stream) + MIDI.
 */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_AUDIO 1
#define CFG_TUD_CDC 1
#define CFG_TUD_MIDI 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_VENDOR 0

/* Audio: microphone only. */
#define SAMPAN_UAC_RATE 48000
#define SAMPAN_UAC_BYTES 2
#define SAMPAN_UAC_CHANNELS 1
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN TUD_AUDIO_MIC_ONE_CH_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX SAMPAN_UAC_BYTES
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX SAMPAN_UAC_CHANNELS
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE SAMPAN_UAC_RATE
#define CFG_TUD_AUDIO_EP_SZ_IN TUD_AUDIO_EP_SIZE(SAMPAN_UAC_RATE, SAMPAN_UAC_BYTES, SAMPAN_UAC_CHANNELS)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX CFG_TUD_AUDIO_EP_SZ_IN
/* Audio arrives in 5 ms blocks; hold about 20 ms. */
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ 2048

/* CDC: the Sampan stream out, commands in. */
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 4096
#define CFG_TUD_CDC_EP_BUFSIZE 64

#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 64

#endif
