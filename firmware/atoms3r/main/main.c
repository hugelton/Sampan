/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * Sampan on AtomS3R + Echo Base:
 *   microphone -> DC cut, gain -> level meter + waveform on the screen
 *                              -> USB microphone (UAC2, 48 kHz)
 *                              -> Sampan stream (ADPCM or PCM) on the USB serial port
 *   sampler: hold the button (long press) to record, release to keep the take
 *            (trimmed, levelled, saved as WAV); click plays it on the speaker.
 *   button -> MIDI Control Change on USB and BLE as well; double click = next palette.
 *   MIDI in (USB or BLE, channel 1): note on plays the take at that pitch
 *            (60 = as recorded); CC 85 127/0 = record / stop, CC 86 = play,
 *            CC 87 = playback volume, CC 88 = microphone gain (0..24 dB).
 * Once a second an INFO packet on the stream reports what the board found and
 * the levels. Opening the serial port at 1200 baud and closing it restarts the
 * board into its ROM loader for flashing.
 * MIDI bridge: what arrives over BLE goes out on USB and the other way round,
 * so a USB host (a computer, or a synth acting as host) can be played from a
 * phone or tablet.
 */
#include <stdio.h>
#include <string.h>

#include "ble_midi.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "es8311.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "sampan/button.h"
#include "sampan/midi.h"
#include "sampan/dsp.h"
#include "sampan/proto.h"
#include "sampan/scope.h"
#include "sampan/tx.h"
#include "sampler.h"
#include "usb.h"

#define RATE 48000
#define BLOCK 240 /* 5 ms */
#define FW_VERSION "0.1.0"

static const char *const lcd_names[] = {"?", "GC9107", "ST7735S"};

/* Safe mode: after two crashes in a row, start without the newer parts
 * (speaker, sampler) so USB still comes up and the board can be reflashed
 * from the computer. The count survives resets but not power-off, and is
 * cleared once the board has run for ten seconds. */
static RTC_NOINIT_ATTR uint32_t crash_count;
static bool safe_mode;

static void check_crashes(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT)
        crash_count++;
    else
        crash_count = 0;
    safe_mode = crash_count >= 2;
}

static struct {
    enum lcd_kind lcd;
    uint32_t lcd_id;
    bool backlight, codec, codec_mic, speaker, sampler;
    uint16_t codec_id;
} found;

/* Settings changed by the UI task, read by the audio task. */
static volatile uint8_t palette = SAMPAN_PAL_DEFAULT;
static volatile uint8_t codec = SAMPAN_CODEC_ADPCM;
static volatile int8_t gain_db = 0;

/* Audio task results, read by the UI task. */
static struct sampan_scope scope;
static struct sampan_meter meter;
static volatile float rms_left, rms_right;
static volatile uint32_t i2s_errors;

static i2s_chan_handle_t rx, tx_i2s;
static struct sampan_tx tx;
static uint8_t frame[SAMPAN_MAX_FRAME];

static bool i2s_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_frame_num = BLOCK;
    cc.auto_clear = true; /* silence if playback falls behind */
    if (i2s_new_channel(&cc, &tx_i2s, &rx) != ESP_OK)
        return false;
    /* 16-bit stereo slots: BCLK = 32 fs, which the codec multiplies up. */
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
        },
    };
    return i2s_channel_init_std_mode(tx_i2s, &sc) == ESP_OK &&
           i2s_channel_init_std_mode(rx, &sc) == ESP_OK && i2s_channel_enable(tx_i2s) == ESP_OK &&
           i2s_channel_enable(rx) == ESP_OK;
}

static void stream(const uint8_t *b, size_t n)
{
    if (usb_stream_open())
        usb_stream_write(b, n); /* never blocks; counts drops */
}

static void send_info(void)
{
    char s[600];
    struct sampler_status ss;
    sampler_status(&ss);
    struct usb_stats us;
    usb_stats(&us);
    struct ble_midi_stats bs;
    ble_midi_stats(&bs);
    int sec_rc, enc_st;
    ble_midi_security(&sec_rc, &enc_st);
    int n = snprintf(s, sizeof(s),
                     "sampan atoms3r %s%s | lcd %s id %06lx | backlight %s | codec %s id %04x mic %s | "
                     "spk %s | %s gain %d host %.0f%s dB | rms L %.1f R %.1f peak %.1f dBFS clip %d | "
                     "uac %s drops %lu (set %lu itf %u alt %u close %lu writes %lu) | stream drops %lu | midi drops %lu | i2s err %lu | "
                     "ble %s%s%s sec %d enc %d mtu %u itvl %.2f ms (updates %lu) rx %lu tx %lu drops %lu bad %lu | "
                     "lat ble->usb avg %lu max %lu us, usb->ble avg %lu max %lu us | "
                     "smp %s%s%s%s takes %lu last %.2f s rec %.2f s free %lu kB err %d psram %s",
                     FW_VERSION, safe_mode ? " SAFE MODE" : "", lcd_names[found.lcd], (unsigned long)(found.lcd_id & 0xFFFFFF),
                     found.backlight ? "ok" : "NO", found.codec ? "ok" : "NO", found.codec_id,
                     found.codec_mic ? "on" : "off", found.speaker ? "on" : "off",
                     codec == SAMPAN_CODEC_PCM16 ? "pcm" : "adpcm",
                     gain_db, usb_mic_volume_db(), usb_mic_muted() ? " muted" : "",
                     sampan_dbfs(rms_left), sampan_dbfs(rms_right), sampan_dbfs(meter.peak),
                     meter.clip, usb_mic_open() ? "open" : "idle", (unsigned long)us.mic_drops,
                     (unsigned long)us.itf_sets, us.last_itf, us.last_alt, (unsigned long)us.itf_closes,
                     (unsigned long)us.mic_writes,
                     (unsigned long)us.stream_drops, (unsigned long)us.midi_drops,
                     (unsigned long)i2s_errors, ble_midi_connected() ? "connected" : "waiting",
                     ble_midi_encrypted() ? " encrypted" : "", ble_midi_bonded() ? " bonded" : "", sec_rc, enc_st,
                     ble_midi_mtu(), ble_midi_interval_ms(), (unsigned long)ble_midi_param_updates(),
                     (unsigned long)bs.rx_msgs, (unsigned long)bs.tx_msgs,
                     (unsigned long)bs.tx_drops, (unsigned long)bs.rx_bad,
                     (unsigned long)us.midi_lat_avg_us, (unsigned long)us.midi_lat_max_us,
                     (unsigned long)bs.tx_lat_avg_us, (unsigned long)bs.tx_lat_max_us,
                     ss.ready ? "ready" : "OFF", ss.recording ? " rec" : "", ss.playing ? " play" : "",
                     ss.saving ? " saving" : "", (unsigned long)ss.takes, ss.last_s, ss.rec_s,
                     (unsigned long)ss.free_kb, ss.last_error, ss.psram ? "ok" : "NO");
    size_t len = sampan_tx_packet(&tx, SAMPAN_TYPE_INFO, (const uint8_t *)s, (size_t)n, frame,
                                  sizeof(frame));
    stream(frame, len);
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t raw[BLOCK * 2], mono[BLOCK], play[BLOCK], out[BLOCK * 2];
    struct sampan_dccut dc;
    struct sampan_gain gain;
    sampan_dccut_init(&dc, 20, RATE);
    sampan_gain_init(&gain, 0, RATE / 100);
    sampan_meter_init(&meter, RATE, 1.0f, 20.0f);
    float gain_now = 0;
    int64_t next_info = esp_timer_get_time() + 1000000;

    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK ||
            got != sizeof(raw)) {
            i2s_errors++;
            continue;
        }
        double sl = 0, sr = 0;
        for (int i = 0; i < BLOCK; i++) {
            mono[i] = raw[2 * i];
            sl += (double)raw[2 * i] * raw[2 * i];
            sr += (double)raw[2 * i + 1] * raw[2 * i + 1];
        }
        rms_left = (float)(__builtin_sqrt(sl / BLOCK) / 32768.0);
        rms_right = (float)(__builtin_sqrt(sr / BLOCK) / 32768.0);

        /* Gain: the device setting plus the volume the host set for the mic. */
        float want = gain_db + usb_mic_volume_db();
        if (want != gain_now) {
            gain_now = want;
            sampan_gain_set(&gain, gain_now);
        }
        sampan_input_process(&dc, &gain, mono, BLOCK);
        sampan_meter_update(&meter, mono, BLOCK);
        sampan_scope_push(&scope, mono, BLOCK, 1);
        sampler_input(mono, BLOCK);

        /* Speaker: the sampler, same block size, so in and out stay in step. */
        memset(play, 0, sizeof(play));
        sampler_render(play, BLOCK);
        for (int i = 0; i < BLOCK; i++)
            out[2 * i] = out[2 * i + 1] = play[i];
        size_t wrote;
        i2s_channel_write(tx_i2s, out, sizeof(out), &wrote, 0);

        if (usb_mic_muted()) {
            static const int16_t silence[BLOCK];
            usb_mic_write(silence, BLOCK);
        } else {
            usb_mic_write(mono, BLOCK);
        }

        size_t n = sampan_tx_audio(&tx, codec, 1, RATE, mono, BLOCK, frame, sizeof(frame));
        stream(frame, n);

        if (esp_timer_get_time() >= next_info) {
            next_info += 1000000;
            send_info();
            if (esp_timer_get_time() > 10000000)
                crash_count = 0;
        }
    }
}

/* Bottom strip: level bar over -60..0 dBFS, peak tick, white when clipping. */
static void draw_meter(uint16_t *px, uint16_t rows, const struct sampan_palette *p)
{
    float db = sampan_dbfs(meter.rms), pk = sampan_dbfs(meter.peak);
    int bar = (int)((db + 60.0f) * LCD_W / 60.0f), tick = (int)((pk + 60.0f) * LCD_W / 60.0f);
    struct sampler_status ss;
    sampler_status(&ss);
    /* Recording: the strip's background turns to the red palette's step 2. */
    uint16_t on = meter.clip ? SAMPAN_WHITE : p->c[3], hi = p->c[4];
    uint16_t dim = ss.recording ? sampan_palettes[SAMPAN_PAL_RED].c[2] : ss.playing ? p->c[1] : p->c[0];
    for (uint16_t r = 0; r < rows; r++)
        for (int x = 0; x < LCD_W; x++) {
            uint16_t c = r == 0 ? SAMPAN_BLACK : x < bar ? on : (x == tick || x == tick - 1) ? hi : dim;
            px[r * LCD_W + x] = (uint16_t)(c >> 8 | c << 8);
        }
}

#define SCOPE_H 118
#define METER_H 10
#define BAND 16

static void ui_task(void *arg)
{
    (void)arg;
    static uint16_t band[LCD_W * BAND];
    static struct sampan_scope_spans sp;
    struct sampan_btn btn;
    sampan_btn_init(&btn, NULL);
    struct sampan_btnmap map;
    sampan_btnmap_default(&map, 0);
    struct sampan_scope_style st;
    TickType_t last_draw = 0;

    for (;;) {
        uint8_t ev[SAMPAN_BTN_MAX_EVENTS];
        int n = sampan_btn_update(&btn, board_button(), (uint32_t)(esp_timer_get_time() / 1000), ev);
        for (int i = 0; i < n; i++) {
            struct sampan_midi_msg m[2];
            int k = sampan_btnmap_event(&map, ev[i], m);
            for (int j = 0; j < k; j++) {
                uint8_t pkt[4];
                if (sampan_usbmidi_pack(0, &m[j], pkt))
                    usb_midi_send(pkt);
                if (ble_midi_connected())
                    ble_midi_send(&m[j]);
            }
            if (ev[i] == SAMPAN_BTN_CLICK)
                sampler_play(0, 1.0f);
            else if (ev[i] == SAMPAN_BTN_DOUBLE)
                palette = (uint8_t)((palette + 1) % SAMPAN_PAL_COUNT);
            else if (ev[i] == SAMPAN_BTN_LONG)
                sampler_rec_start();
            else if (ev[i] == SAMPAN_BTN_RELEASE)
                sampler_rec_stop(); /* no-op unless recording */
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_draw >= pdMS_TO_TICKS(33)) {
            last_draw = now;
            sampan_scope_style_default(&st, palette);
            st.swap_bytes = true;
            const struct sampan_scope_frame *f = sampan_scope_latest(&scope, NULL);
            if (f) {
                sampan_scope_spans(f, SCOPE_H, &st, &sp);
                for (uint16_t y = 0; y < SCOPE_H; y += BAND) {
                    uint16_t rows = SCOPE_H - y < BAND ? SCOPE_H - y : BAND;
                    sampan_scope_render(&sp, &st, y, rows, band);
                    lcd_rows(y, rows, band);
                }
            }
            draw_meter(band, METER_H, &sampan_palettes[palette % SAMPAN_PAL_COUNT]);
            lcd_rows(SCOPE_H, METER_H, band);
        }
        vTaskDelay(1);
    }
}

/* MIDI that controls Sampan itself (channel 1). */
static void control(const struct sampan_midi_msg *m)
{
    if (m->sysex || m->len < 3 || (m->b[0] & 0x0F) != 0)
        return;
    uint8_t st = m->b[0] & 0xF0, d1 = m->b[1], d2 = m->b[2];
    if (st == 0x90 && d2)
        sampler_play((float)d1 - 60.0f, d2 / 127.0f);
    else if (st == 0xB0 && d1 == 85)
        d2 ? sampler_rec_start() : sampler_rec_stop();
    else if (st == 0xB0 && d1 == 86 && d2)
        sampler_play(0, 1.0f);
    else if (st == 0xB0 && d1 == 87)
        sampler_set_volume(d2 / 127.0f);
    else if (st == 0xB0 && d1 == 88)
        gain_db = (int8_t)(d2 * 24 / 127);
}

/* BLE -> USB (runs in the BLE host task). */
static void from_ble(const struct sampan_midi_msg *m)
{
    control(m);
    uint8_t pkt[4];
    if (sampan_usbmidi_pack(0, m, pkt))
        usb_midi_send(pkt);
}

/* USB -> BLE (runs in the USB task). */
static void from_usb(const uint8_t pkt[4])
{
    static struct sampan_midi_parser p;
    uint8_t raw[3];
    int n = sampan_usbmidi_unpack(pkt, raw);
    for (int i = 0; i < n; i++) {
        struct sampan_midi_msg m[2];
        int k = sampan_midi_parse(&p, raw[i], m);
        for (int j = 0; j < k; j++) {
            control(&m[j]);
            if (ble_midi_connected())
                ble_midi_send(&m[j]);
        }
    }
}

void app_main(void)
{
    check_crashes();
    found.backlight = board_init();
    found.lcd = lcd_init(&found.lcd_id);
    board_backlight(160);

    usb_midi_on_receive(from_usb);
    usb_start();
    ble_midi_start("Sampan", from_ble);
    sampan_tx_init(&tx);

    struct sampan_scope_cfg cfg;
    sampan_scope_cfg_default(&cfg, LCD_W, RATE);
    sampan_scope_init(&scope, &cfg);

    found.codec = es8311_open(board.ext, &found.codec_id);
    bool i2s = i2s_start();
    if (found.codec && i2s) {
        found.codec_mic = es8311_mic_on(10, 0xBF); /* PGA +30 dB, ADC 0 dB */
        if (!safe_mode)
            found.speaker = es8311_speaker_on(0xBF); /* DAC 0 dB; level set in software */
    }
    if (!safe_mode)
        found.sampler = sampler_init();

    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 5, NULL, 0);
}
