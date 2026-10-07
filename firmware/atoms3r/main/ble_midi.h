/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * BLE MIDI peripheral (the standard MIDI-over-Bluetooth-LE service).
 */
#ifndef SAMPAN_BLE_MIDI_H
#define SAMPAN_BLE_MIDI_H

#include <stdbool.h>
#include <stdint.h>

#include "sampan/midi.h"

/* Called from the BLE host task for every message received. */
typedef void (*ble_midi_rx_fn)(const struct sampan_midi_msg *m);

void ble_midi_start(const char *name, ble_midi_rx_fn rx);

bool ble_midi_connected(void); /* connected and notifications on */
uint16_t ble_midi_mtu(void);
float ble_midi_interval_ms(void);     /* the connection interval in use, 0 if none */
uint32_t ble_midi_param_updates(void); /* connection parameter changes so far */
bool ble_midi_encrypted(void);
bool ble_midi_bonded(void);
/* Last security request result and encryption change status (-1: none yet). */
void ble_midi_security(int *initiate_rc, int *enc_change_status);

/* From any task. Returns false when dropped (not connected or queue full). */
bool ble_midi_send(const struct sampan_midi_msg *m);

struct ble_midi_stats {
    uint32_t rx_msgs, tx_msgs, tx_drops, rx_bad;
    uint32_t tx_lat_avg_us, tx_lat_max_us; /* queue to notification, since the last call */
};
void ble_midi_stats(struct ble_midi_stats *s);

#endif
