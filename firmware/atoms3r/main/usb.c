/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Composite USB device. The audio control handling follows the UAC2 spec
 * (clock source, input terminal, feature unit with mute and volume), in the
 * shape TinyUSB's examples use (MIT licence).
 */
#include "usb.h"

#include <string.h>

#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sampan/ring.h"
#include "soc/rtc_cntl_reg.h"
#include "tusb.h"

/* ---- descriptors --------------------------------------------------------- */

/* Development IDs (pid.codes test VID/PID); a real PID before any release. */
#define USB_VID 0x1209
#define USB_PID 0x0001

enum {
    ITF_AUDIO_CONTROL,
    ITF_AUDIO_STREAMING,
    ITF_CDC,
    ITF_CDC_DATA,
    ITF_MIDI,
    ITF_MIDI_STREAMING,
    ITF_TOTAL,
};

#define EP_AUDIO_IN 0x81
#define EP_CDC_NOTIF 0x82
#define EP_CDC_OUT 0x03
#define EP_CDC_IN 0x83
#define EP_MIDI_OUT 0x04
#define EP_MIDI_IN 0x84

enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_MIC, STR_STREAM, STR_MIDI };

/* Audio entity IDs fixed by TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR. */
#define UAC_INPUT_TERMINAL 1
#define UAC_FEATURE_UNIT 2
#define UAC_CLOCK 4

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC, /* interface association */
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0001,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_AUDIO_MIC_ONE_CH_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
    TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR(ITF_AUDIO_CONTROL, STR_MIC, SAMPAN_UAC_BYTES,
                                    SAMPAN_UAC_BYTES * 8, EP_AUDIO_IN, CFG_TUD_AUDIO_EP_SZ_IN),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_STREAM, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MIDI_DESCRIPTOR(ITF_MIDI, STR_MIDI, EP_MIDI_OUT, EP_MIDI_IN, 64),
};

static char serial[13];
static const char *const strings[] = {
    [STR_MANUFACTURER] = "Hügelton Instruments",
    [STR_PRODUCT] = "Sampan",
    [STR_SERIAL] = serial,
    [STR_MIC] = "Sampan",
    [STR_STREAM] = "Sampan Stream",
    [STR_MIDI] = "Sampan MIDI",
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device_desc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return config_desc;
}

/* UTF-8 (BMP only) to the UTF-16 string descriptor. */
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t d[40];
    uint8_t n = 0;
    if (index == STR_LANG) {
        d[1] = 0x0409;
        n = 1;
    } else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) {
        const uint8_t *s = (const uint8_t *)strings[index];
        while (*s && n < 39) {
            uint16_t c = *s++;
            if ((c & 0xE0) == 0xC0 && *s)
                c = (uint16_t)((c & 0x1F) << 6 | (*s++ & 0x3F));
            else if ((c & 0xF0) == 0xE0 && s[0] && s[1]) {
                c = (uint16_t)((c & 0x0F) << 12 | (s[0] & 0x3F) << 6 | (s[1] & 0x3F));
                s += 2;
            }
            d[1 + n++] = c;
        }
    } else {
        return NULL;
    }
    d[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
    return d;
}

/* ---- state ---------------------------------------------------------------- */

static volatile bool mic_open;
static volatile uint32_t itf_sets, itf_closes, mic_writes;
static volatile uint8_t last_itf, last_alt;
static bool mute[2];
static int16_t volume[2]; /* 1/256 dB, master and channel 1 */
static const uint32_t sample_rate = SAMPAN_UAC_RATE;

static struct sampan_ring stream_ring;
static uint8_t stream_buf[16384];
static QueueHandle_t midi_q;
struct midi_item {
    uint8_t pkt[4];
    uint32_t t_us; /* when it was queued, for the latency figures */
};
static volatile uint32_t midi_lat_max_us, midi_lat_sum_us, midi_lat_n;
static volatile struct usb_stats stats;
static volatile bool want_download;
static usb_midi_rx_fn midi_rx;

/* ---- audio class callbacks ------------------------------------------------ */

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
    itf_sets++;
    last_itf = tu_u16_low(req->wIndex);
    last_alt = tu_u16_low(req->wValue);
    if (tu_u16_low(req->wIndex) == ITF_AUDIO_STREAMING)
        mic_open = tu_u16_low(req->wValue) != 0;
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    (void)rhport;
    itf_closes++;
    if (tu_u16_low(req->wIndex) == ITF_AUDIO_STREAMING)
        mic_open = false;
    return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    (void)rhport;
    uint8_t ch = tu_u16_low(req->wValue), sel = tu_u16_high(req->wValue);
    if (tu_u16_high(req->wIndex) != UAC_FEATURE_UNIT || req->bRequest != AUDIO_CS_REQ_CUR || ch > 1)
        return false;
    if (sel == AUDIO_FU_CTRL_MUTE) {
        mute[ch] = buf[0] != 0;
        return true;
    }
    if (sel == AUDIO_FU_CTRL_VOLUME) {
        memcpy(&volume[ch], buf, 2);
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t ch = tu_u16_low(req->wValue), sel = tu_u16_high(req->wValue);
    uint8_t entity = tu_u16_high(req->wIndex);

    if (entity == UAC_INPUT_TERMINAL && sel == AUDIO_TE_CTRL_CONNECTOR) {
        audio_desc_channel_cluster_t c = {.bNrChannels = 1, .bmChannelConfig = 0, .iChannelNames = 0};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &c, sizeof(c));
    }
    if (entity == UAC_FEATURE_UNIT && ch <= 1) {
        if (sel == AUDIO_FU_CTRL_MUTE)
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &mute[ch], 1);
        if (sel == AUDIO_FU_CTRL_VOLUME && req->bRequest == AUDIO_CS_REQ_CUR)
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &volume[ch], 2);
        if (sel == AUDIO_FU_CTRL_VOLUME && req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) r = {
                .wNumSubRanges = 1,
                .subrange[0] = {.bMin = -60 * 256, .bMax = 30 * 256, .bRes = 256},
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof(r));
        }
    }
    if (entity == UAC_CLOCK) {
        if (sel == AUDIO_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO_CS_REQ_CUR)
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, (void *)&sample_rate, 4);
        if (sel == AUDIO_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(1) r = {
                .wNumSubRanges = 1,
                .subrange[0] = {.bMin = SAMPAN_UAC_RATE, .bMax = SAMPAN_UAC_RATE, .bRes = 0},
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof(r));
        }
        if (sel == AUDIO_CS_CTRL_CLK_VALID) {
            uint8_t valid = 1;
            return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &valid, 1);
        }
    }
    return false;
}

/* ---- CDC: a 1200 baud open-and-close reboots into the ROM loader ----------- */

static bool baud_1200;

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    baud_1200 = coding->bit_rate == 1200;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (!dtr && baud_1200)
        want_download = true;
}

/* Hand the USB pins back to the built-in serial/JTAG port, which the ROM
 * loader uses, and restart into download mode. */
static void reboot_to_download(void)
{
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    esp_restart();
}

/* ---- task ------------------------------------------------------------------ */

static void usb_task(void *arg)
{
    (void)arg;
    tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
    tusb_init(0, &init);
    uint8_t chunk[64], pkt[4];

    for (;;) {
        tud_task_ext(1, false);
        if (want_download)
            reboot_to_download();

        /* Sampan stream: only while a program holds the port open. */
        if (tud_cdc_connected()) {
            uint32_t room = tud_cdc_write_available();
            while (room) {
                size_t n = sampan_ring_read(&stream_ring, chunk, room < sizeof(chunk) ? room : sizeof(chunk));
                if (!n)
                    break;
                tud_cdc_write(chunk, n);
                room -= (uint32_t)n;
            }
            tud_cdc_write_flush();
        } else {
            while (sampan_ring_read(&stream_ring, chunk, sizeof(chunk)))
                ;
        }
        while (tud_cdc_available())
            tud_cdc_read(chunk, sizeof(chunk)); /* no commands yet */

        struct midi_item it;
        while (xQueueReceive(midi_q, &it, 0) == pdTRUE) {
            if (!tud_midi_mounted() || !tud_midi_packet_write(it.pkt)) {
                stats.midi_drops++;
                continue;
            }
            uint32_t d = (uint32_t)esp_timer_get_time() - it.t_us;
            if (d > midi_lat_max_us)
                midi_lat_max_us = d;
            midi_lat_sum_us += d;
            midi_lat_n++;
        }
        while (tud_midi_available() && tud_midi_packet_read(pkt))
            if (midi_rx)
                midi_rx(pkt);
    }
}

void usb_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    for (int i = 0; i < 6; i++)
        sprintf(serial + 2 * i, "%02X", mac[i]);

    sampan_ring_init(&stream_ring, stream_buf, sizeof(stream_buf));
    midi_q = xQueueCreate(32, sizeof(struct midi_item));

    static usb_phy_handle_t phy;
    usb_phy_config_t pc = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    usb_new_phy(&pc, &phy);
    xTaskCreatePinnedToCore(usb_task, "usb", 4096, NULL, 12, NULL, 0);
}

bool usb_mounted(void)
{
    return tud_mounted();
}

bool usb_mic_open(void)
{
    return mic_open;
}

bool usb_stream_open(void)
{
    return tud_cdc_connected();
}

bool usb_mic_write(const int16_t *x, size_t n)
{
    if (!mic_open)
        return true;
    uint16_t want = (uint16_t)(n * 2);
    mic_writes++;
    if (tud_audio_write(x, want) != want) {
        stats.mic_drops++;
        return false;
    }
    return true;
}

bool usb_stream_write(const uint8_t *b, size_t n)
{
    if (sampan_ring_free(&stream_ring) < n) {
        stats.stream_drops++;
        return false;
    }
    sampan_ring_write(&stream_ring, b, n);
    return true;
}

bool usb_midi_send(const uint8_t pkt[4])
{
    struct midi_item it = {.t_us = (uint32_t)esp_timer_get_time()};
    memcpy(it.pkt, pkt, 4);
    if (xQueueSend(midi_q, &it, 0) != pdTRUE) {
        stats.midi_drops++;
        return false;
    }
    return true;
}

void usb_midi_on_receive(usb_midi_rx_fn fn)
{
    midi_rx = fn;
}

float usb_mic_volume_db(void)
{
    return (volume[0] + volume[1]) / 256.0f;
}

bool usb_mic_muted(void)
{
    return mute[0] || mute[1];
}

void usb_stats(struct usb_stats *s)
{
    s->mic_drops = stats.mic_drops;
    s->stream_drops = stats.stream_drops;
    s->midi_drops = stats.midi_drops;
    s->itf_sets = itf_sets;
    s->itf_closes = itf_closes;
    s->mic_writes = mic_writes;
    s->last_itf = last_itf;
    s->last_alt = last_alt;
    s->midi_lat_max_us = midi_lat_max_us;
    s->midi_lat_avg_us = midi_lat_n ? midi_lat_sum_us / midi_lat_n : 0;
    midi_lat_max_us = midi_lat_sum_us = midi_lat_n = 0;
}
