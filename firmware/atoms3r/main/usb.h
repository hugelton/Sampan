/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * The USB device: UAC2 microphone, CDC carrying the Sampan stream, MIDI.
 * All TinyUSB calls happen in the USB task; other tasks hand data over
 * through rings and a queue.
 */
#ifndef SAMPAN_USB_H
#define SAMPAN_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usb_start(void);

bool usb_mounted(void);
bool usb_mic_open(void);    /* the host is recording from the microphone */
bool usb_stream_open(void); /* a program has the serial port open */

/* From the audio task. Each returns false when the data had to be dropped. */
bool usb_mic_write(const int16_t *x, size_t n);
bool usb_stream_write(const uint8_t *b, size_t n);

/* From any task: one USB-MIDI event packet. */
bool usb_midi_send(const uint8_t pkt[4]);

/* Called in the USB task for each USB-MIDI event packet from the host. */
typedef void (*usb_midi_rx_fn)(const uint8_t pkt[4]);
void usb_midi_on_receive(usb_midi_rx_fn fn);

/* Microphone volume the host set (dB) and its mute. */
float usb_mic_volume_db(void);
bool usb_mic_muted(void);

struct usb_stats {
    uint32_t mic_drops, stream_drops, midi_drops;
    uint32_t midi_lat_avg_us, midi_lat_max_us; /* queue to USB endpoint, since the last call */
    uint32_t itf_sets, itf_closes, mic_writes; /* audio interface diagnostics */
    uint8_t last_itf, last_alt;
};
void usb_stats(struct usb_stats *s);

#endif
