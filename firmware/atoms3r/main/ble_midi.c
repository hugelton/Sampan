/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * One GATT service with one characteristic (read: empty, write without
 * response: incoming MIDI, notify: outgoing MIDI), packets framed by
 * core/midi. Outgoing messages queue up and go out in as few notifications
 * as the negotiated MTU allows.
 *
 * Bonding: on connect Sampan asks for encryption (Just Works, no passkey);
 * the keys are kept in NVS so the host can reconnect by itself later.
 */
#include "ble_midi.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, little-endian */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
/* 7772E5DB-3868-4112-A1A9-F2669D106BF3, little-endian */
static const ble_uuid128_t chr_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static uint16_t chr_handle;
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool notify_on;
static volatile uint16_t mtu = 23;
static volatile uint16_t itvl; /* connection interval, 1.25 ms units */
static volatile uint32_t updates;
static volatile bool encrypted, bonded;
static volatile int sec_rc = -1, enc_status = -1; /* diagnostics */

void ble_store_config_init(void);
static uint8_t own_addr_type;
static ble_midi_rx_fn rx_cb;
static struct sampan_midi_parser rx_parser;
static QueueHandle_t tx_q;
static volatile uint32_t tx_lat_max_us, tx_lat_sum_us, tx_lat_n;

struct tx_item {
    struct sampan_midi_msg m;
    uint32_t t_us; /* when it was queued */
};
static volatile struct ble_midi_stats stats;

static int chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                      void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return 0; /* the spec: reads return no payload */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;

    uint8_t pkt[SAMPAN_BLEMIDI_MAX + 8], raw[SAMPAN_BLEMIDI_MAX + 8];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, pkt, sizeof(pkt), &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    int n = sampan_blemidi_decode(pkt, len, raw, sizeof(raw));
    if (n < 0) {
        stats.rx_bad++;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        struct sampan_midi_msg m[2];
        int k = sampan_midi_parse(&rx_parser, raw[i], m);
        for (int j = 0; j < k; j++) {
            stats.rx_msgs++;
            if (rx_cb)
                rx_cb(&m[j]);
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &chr_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &chr_handle,
            },
            {0},
        },
    },
    {0},
};

static void advertise(void);

static void read_itvl(void)
{
    struct ble_gap_conn_desc d;
    if (conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn, &d) == 0)
        itvl = d.conn_itvl;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            advertise();
            break;
        }
        conn = ev->connect.conn_handle;
        read_itvl();
        sec_rc = ble_gap_security_initiate(conn);
        ble_gattc_exchange_mtu(conn, NULL, NULL);
        {
            /* Short interval for playable latency, within what Apple hosts
             * accept: min >= 11.25 ms and max >= min + 15 ms. */
            struct ble_gap_upd_params p = {
                .itvl_min = 9,  /* 11.25 ms */
                .itvl_max = 21, /* 26.25 ms */
                .latency = 0,
                .supervision_timeout = 400,
            };
            ble_gap_update_params(conn, &p);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        notify_on = false;
        itvl = 0;
        encrypted = bonded = false;
        mtu = 23;
        sampan_midi_parser_init(&rx_parser);
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == chr_handle)
            notify_on = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        enc_status = ev->enc_change.status;
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            encrypted = d.sec_state.encrypted;
            bonded = d.sec_state.bonded;
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The host lost its keys (or was reset): forget ours and pair again. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
            ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_CONN_UPDATE:
        updates++;
        read_itvl();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0)
        return;

    struct ble_hs_adv_fields r = {0};
    const char *name = ble_svc_gap_device_name();
    r.name = (uint8_t *)name;
    r.name_len = (uint8_t)strlen(name);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void flush(struct sampan_blemidi_tx *tx, uint32_t oldest_us)
{
    if (tx->len && conn != BLE_HS_CONN_HANDLE_NONE && notify_on) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(tx->buf, tx->len);
        if (!om || ble_gatts_notify_custom(conn, chr_handle, om) != 0)
            stats.tx_drops++;
        /* time from the oldest message in this packet being queued */
        uint32_t d = (uint32_t)esp_timer_get_time() - oldest_us;
        if (d > tx_lat_max_us)
            tx_lat_max_us = d;
        tx_lat_sum_us += d;
        tx_lat_n++;
    }
    sampan_blemidi_tx_reset(tx);
}

/* Gather whatever is queued into as few notifications as fit the MTU. */
static void tx_task(void *arg)
{
    (void)arg;
    static struct sampan_blemidi_tx tx;
    struct tx_item it;
    for (;;) {
        if (xQueueReceive(tx_q, &it, portMAX_DELAY) != pdTRUE)
            continue;
        sampan_blemidi_tx_init(&tx, mtu - 3u);
        uint32_t oldest = it.t_us;
        bool have = true;
        while (have) {
            uint16_t ts = (uint16_t)(esp_timer_get_time() / 1000);
            if (!sampan_blemidi_add(&tx, &it.m, ts)) {
                flush(&tx, oldest); /* full, or the clock moved on: it goes in the next one */
                oldest = it.t_us;
                continue;
            }
            have = xQueueReceive(tx_q, &it, 0) == pdTRUE;
        }
        flush(&tx, oldest);
    }
}

void ble_midi_start(const char *name, ble_midi_rx_fn rx)
{
    rx_cb = rx;
    sampan_midi_parser_init(&rx_parser);
    tx_q = xQueueCreate(64, sizeof(struct tx_item));

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    if (nimble_port_init() != ESP_OK)
        return;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(services);
    ble_gatts_add_svcs(services);
    ble_svc_gap_device_name_set(name);
    ble_att_set_preferred_mtu(SAMPAN_BLEMIDI_MAX + 3);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    xTaskCreatePinnedToCore(tx_task, "blemidi", 3072, NULL, 9, NULL, 0);
}

bool ble_midi_connected(void)
{
    return conn != BLE_HS_CONN_HANDLE_NONE && notify_on;
}

uint16_t ble_midi_mtu(void)
{
    return mtu;
}

float ble_midi_interval_ms(void)
{
    return itvl * 1.25f;
}

bool ble_midi_encrypted(void)
{
    return encrypted;
}

bool ble_midi_bonded(void)
{
    return bonded;
}

void ble_midi_security(int *initiate_rc, int *enc_change_status)
{
    *initiate_rc = sec_rc;
    *enc_change_status = enc_status;
}

uint32_t ble_midi_param_updates(void)
{
    return updates;
}

bool ble_midi_send(const struct sampan_midi_msg *m)
{
    struct tx_item it = {.m = *m, .t_us = (uint32_t)esp_timer_get_time()};
    if (!ble_midi_connected() || !tx_q || xQueueSend(tx_q, &it, 0) != pdTRUE) {
        stats.tx_drops++;
        return false;
    }
    stats.tx_msgs++;
    return true;
}

void ble_midi_stats(struct ble_midi_stats *s)
{
    s->rx_msgs = stats.rx_msgs;
    s->tx_msgs = stats.tx_msgs;
    s->tx_drops = stats.tx_drops;
    s->rx_bad = stats.rx_bad;
    s->tx_lat_max_us = tx_lat_max_us;
    s->tx_lat_avg_us = tx_lat_n ? tx_lat_sum_us / tx_lat_n : 0;
    tx_lat_max_us = tx_lat_sum_us = tx_lat_n = 0;
}
