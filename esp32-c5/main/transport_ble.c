/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Stub when CONFIG_BT_ENABLED=n. Full implementation in git. */
#include "sdkconfig.h"
#include "transport_ble.h"

#ifndef CONFIG_BT_ENABLED
bool transport_ble_init(void) { return false; }
uint32_t transport_ble_connected_count(void) { return 0; }
void transport_ble_pause_adv(void) {}
void transport_ble_resume_adv(void) {}
bool transport_ble_is_adv_paused(void) { return false; }
#else
/* Full implementation below */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/portmacro.h"

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

#include "io.h"
#include "cli.h"

static const char *TAG = "ble";

/* ============================================================
 * NUS UUIDs
 * ============================================================ */
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

/* ============================================================
 * Konfigurasi
 * ============================================================ */
#define BLE_MTU            247
#define MAX_NOTIFY_PAYLOAD (BLE_MTU - 3)
#define TX_BUF_BYTES       4096
#define BLE_MAX_CONN       3

/* Instance 0 = NUS (extended advertising, connectable) */
#define NUS_ADV_INSTANCE   0

/* ============================================================
 * State GATT connections
 * ============================================================ */
static uint16_t          s_conn_handles[BLE_MAX_CONN];
static bool              s_notify_enabled[BLE_MAX_CONN];
static char              s_rx_lines[BLE_MAX_CONN][256];
static size_t            s_rx_lengths[BLE_MAX_CONN];
static bool              s_rx_discarding[BLE_MAX_CONN];
static uint16_t          s_tx_attr_handle = 0;
static uint8_t           s_own_addr_type;
static bool              s_addr_inferred;
static StreamBufferHandle_t s_tx_sb;
static struct ble_npl_callout s_adv_callout;
static portMUX_TYPE s_conn_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_drop_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_tx_dropped_bytes;

/* Extended adv state */
static bool s_nus_adv_active = false;
static volatile bool s_adv_paused = false;

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_advertising(void);

/* ============================================================
 * GATT callbacks
 * ============================================================ */
static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) return 0;
    char buf[256];
    if (len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0)
        return BLE_ATT_ERR_UNLIKELY;

    int slot = -1;
    for (int i = 0; i < BLE_MAX_CONN; i++) {
        if (s_conn_handles[i] == conn_handle) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return BLE_ATT_ERR_UNLIKELY;

    for (uint16_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (s_rx_discarding[slot]) {
                s_rx_discarding[slot] = false;
                s_rx_lengths[slot] = 0;
                continue;
            }
            s_rx_lines[slot][s_rx_lengths[slot]++] = '\n';
            cli_feed(s_rx_lines[slot], s_rx_lengths[slot]);
            s_rx_lengths[slot] = 0;
        } else if (s_rx_discarding[slot]) {
            continue;
        } else if (s_rx_lengths[slot] < sizeof(s_rx_lines[slot]) - 1) {
            s_rx_lines[slot][s_rx_lengths[slot]++] = c;
        } else {
            s_rx_lengths[slot] = 0;
            s_rx_discarding[slot] = true;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
    }
    return 0;
}

static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    { .uuid = &NUS_RX_UUID.u, .access_cb = rx_access_cb,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &NUS_TX_UUID.u, .access_cb = tx_access_cb,
      .val_handle = &s_tx_attr_handle,
      .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ },
    { 0 }
};

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &NUS_SVC_UUID.u,
      .characteristics = s_chrs },
    { 0 }
};

/* ============================================================
 * Extended Advertising: build adv data manual
 * ============================================================ */
static int build_nus_adv_data(uint8_t *out, size_t out_size)
{
    if (out_size < 31) return -1;
    size_t len = 0;

    /* Flags: LE General Discoverable, BR/EDR Not Supported */
    out[len++] = 0x02;
    out[len++] = 0x01;
    out[len++] = 0x06;

    /* Complete list of 128-bit Service UUIDs */
    out[len++] = 0x11;   /* AD length (17) */
    out[len++] = 0x07;   /* AD type: Complete 128-bit UUIDs */
    memcpy(out + len, NUS_SVC_UUID.value, 16);
    len += 16;

    /* Complete local name "Radio C5" (fills the remaining 10 bytes). */
    out[len++] = 0x09;
    out[len++] = 0x09;
    out[len++] = 'R';
    out[len++] = 'a';
    out[len++] = 'd';
    out[len++] = 'i';
    out[len++] = 'o';
    out[len++] = ' ';
    out[len++] = 'C';
    out[len++] = '5';

    return (int)len;
}

static int build_nus_rsp_data(uint8_t *out, size_t out_size)
{
    if (out_size < 31) return -1;
    size_t len = 0;

    const char *name = ble_svc_gap_device_name();
    size_t name_len = strlen(name);
    if (name_len > 29) name_len = 29;

    out[len++] = (uint8_t)(name_len + 1);
    out[len++] = 0x09;   /* AD type: Complete Local Name */
    memcpy(out + len, name, name_len);
    len += name_len;

    return (int)len;
}

/* ============================================================
 * Start advertising (extended API)
 * ============================================================ */
static void start_advertising(void)
{
    /* Skip while paused by the BLE scanner. */
    if (s_adv_paused) return;

    struct ble_gap_ext_adv_params params = {0};
    params.own_addr_type  = s_own_addr_type;
    params.primary_phy    = BLE_HCI_LE_PHY_1M;
    params.secondary_phy  = BLE_HCI_LE_PHY_1M;
    params.sid            = 0;   /* NUS = SID 0 (spam pakai SID 1-3) */
    params.connectable    = 1;
    params.scannable      = 1;
    params.legacy_pdu     = 1;
    params.itvl_min       = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max       = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    params.tx_power       = 9;

    int rc = ble_gap_ext_adv_configure(NUS_ADV_INSTANCE, &params,
                                        NULL, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ext_adv_configure rc=%d, retry in 500 ms", rc);
        ble_npl_callout_reset(&s_adv_callout,
                              ble_npl_time_ms_to_ticks32(500));
        return;
    }

    uint8_t adv_data[31];
    int adv_len = build_nus_adv_data(adv_data, sizeof(adv_data));
    if (adv_len < 0) {
        ESP_LOGE(TAG, "build_nus_adv_data failed");
        return;
    }

    struct os_mbuf *adv_om = os_msys_get_pkthdr((uint16_t)adv_len, 0);
    if (!adv_om) {
        ESP_LOGE(TAG, "adv mbuf alloc failed");
        return;
    }
    if (os_mbuf_append(adv_om, adv_data, (uint16_t)adv_len) != 0) {
        os_mbuf_free_chain(adv_om);
        return;
    }

    rc = ble_gap_ext_adv_set_data(NUS_ADV_INSTANCE, adv_om);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ext_adv_set_data rc=%d", rc);
        return;
    }

    uint8_t rsp_data[31];
    int rsp_len = build_nus_rsp_data(rsp_data, sizeof(rsp_data));
    if (rsp_len < 0) {
        ESP_LOGE(TAG, "build_nus_rsp_data failed");
        return;
    }

    struct os_mbuf *rsp_om = os_msys_get_pkthdr((uint16_t)rsp_len, 0);
    if (!rsp_om) {
        ESP_LOGE(TAG, "rsp mbuf alloc failed");
        return;
    }
    if (os_mbuf_append(rsp_om, rsp_data, (uint16_t)rsp_len) != 0) {
        os_mbuf_free_chain(rsp_om);
        return;
    }

    rc = ble_gap_ext_adv_rsp_set_data(NUS_ADV_INSTANCE, rsp_om);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ext_adv_rsp_set_data rc=%d", rc);
        return;
    }

    rc = ble_gap_ext_adv_start(NUS_ADV_INSTANCE, 0, 0);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_nus_adv_active = true;
        ESP_LOGI(TAG, "advertising NUS as Radio C5 (ext adv)");
    } else {
        ESP_LOGW(TAG, "ext_adv_start rc=%d, retry in 200 ms", rc);
        ble_npl_callout_reset(&s_adv_callout,
                              ble_npl_time_ms_to_ticks32(200));
    }
}

/* ============================================================
 * Event callback
 * ============================================================ */
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t h = event->connect.conn_handle;
            int slot = -1;
            portENTER_CRITICAL(&s_conn_lock);
            for (int i = 0; i < BLE_MAX_CONN; i++) {
                if (s_conn_handles[i] == BLE_HS_CONN_HANDLE_NONE) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                portEXIT_CRITICAL(&s_conn_lock);
                ESP_LOGW(TAG, "max connections reached, refusing h=%u", h);
                ble_gap_terminate(h, BLE_ERR_CONN_LIMIT);
            } else {
                s_conn_handles[slot] = h;
                s_notify_enabled[slot] = false;
                s_rx_lengths[slot] = 0;
                s_rx_discarding[slot] = false;
                portEXIT_CRITICAL(&s_conn_lock);
                ESP_LOGI(TAG, "connected slot=%d h=%u", slot, h);
                start_advertising();
            }
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = event->disconnect.conn.conn_handle;
        bool any_connected = false;
        int disconnected_slot = -1;
        portENTER_CRITICAL(&s_conn_lock);
        for (int i = 0; i < BLE_MAX_CONN; i++) {
            if (s_conn_handles[i] == h) {
                s_notify_enabled[i]  = false;
                s_conn_handles[i]    = BLE_HS_CONN_HANDLE_NONE;
                s_rx_lengths[i]      = 0;
                s_rx_discarding[i]   = false;
                disconnected_slot = i;
            }
            if (s_conn_handles[i] != BLE_HS_CONN_HANDLE_NONE)
                any_connected = true;
        }
        portEXIT_CRITICAL(&s_conn_lock);
        if (disconnected_slot >= 0) {
            ESP_LOGI(TAG, "disconnected slot=%d h=%u reason=0x%02x",
                     disconnected_slot, h, event->disconnect.reason);
        }
        if (!any_connected && s_tx_sb) xStreamBufferReset(s_tx_sb);
        if (s_nus_adv_active) {
            ble_gap_ext_adv_stop(NUS_ADV_INSTANCE);
            s_nus_adv_active = false;
        }
        start_advertising();
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_nus_adv_active = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_attr_handle) {
            uint16_t h = event->subscribe.conn_handle;
            int slot = -1;
            portENTER_CRITICAL(&s_conn_lock);
            for (int i = 0; i < BLE_MAX_CONN; i++) {
                if (s_conn_handles[i] == h) {
                    s_notify_enabled[i] = event->subscribe.cur_notify;
                    slot = i;
                    break;
                }
            }
            portEXIT_CRITICAL(&s_conn_lock);
            if (slot >= 0) {
                ESP_LOGI(TAG, "notify slot=%d %s", slot,
                         event->subscribe.cur_notify ? "on" : "off");
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_EXT_DISC:
        return 0;
    }
    return 0;
}

/* ============================================================
 * Sync / Reset callbacks
 * ============================================================ */
static void adv_callout_fn(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_addr_inferred) {
        int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
        if (rc != 0) {
            ESP_LOGE(TAG, "addr_infer rc=%d, retry in 500 ms", rc);
            ble_npl_callout_reset(&s_adv_callout,
                                  ble_npl_time_ms_to_ticks32(500));
            return;
        }
        s_addr_inferred = true;
    }
    start_advertising();
}

static void on_sync(void)
{
    s_addr_inferred = false;
    s_nus_adv_active = false;
    portENTER_CRITICAL(&s_conn_lock);
    for (int i = 0; i < BLE_MAX_CONN; i++) {
        s_conn_handles[i]   = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled[i] = false;
        s_rx_lengths[i]     = 0;
        s_rx_discarding[i]  = false;
    }
    portEXIT_CRITICAL(&s_conn_lock);
    portENTER_CRITICAL(&s_drop_lock);
    s_tx_dropped_bytes = 0;
    portEXIT_CRITICAL(&s_drop_lock);
    adv_callout_fn(NULL);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "reset: %d — restarting advertising in 500 ms", reason);
    portENTER_CRITICAL(&s_conn_lock);
    for (int i = 0; i < BLE_MAX_CONN; i++) {
        s_conn_handles[i]   = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled[i] = false;
        s_rx_lengths[i]     = 0;
        s_rx_discarding[i]  = false;
    }
    portEXIT_CRITICAL(&s_conn_lock);
    s_addr_inferred = false;
    s_nus_adv_active = false;
    ble_npl_callout_reset(&s_adv_callout, ble_npl_time_ms_to_ticks32(500));
}

/* ============================================================
 * Host & TX tasks
 * ============================================================ */
static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    ESP_LOGE(TAG, "NimBLE host task returned");
    nimble_port_freertos_deinit();
}

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t buf[MAX_NOTIFY_PAYLOAD];
    for (;;) {
        size_t n = xStreamBufferReceive(s_tx_sb, buf, sizeof(buf),
                                        portMAX_DELAY);
        if (n == 0) continue;

        portENTER_CRITICAL(&s_drop_lock);
        uint32_t dropped = s_tx_dropped_bytes;
        s_tx_dropped_bytes = 0;
        portEXIT_CRITICAL(&s_drop_lock);
        if (dropped > 0) {
            ESP_LOGW(TAG, "TX queue previously dropped %u bytes",
                     (unsigned)dropped);
        }

        uint16_t conn_handles[BLE_MAX_CONN];
        bool notify_enabled[BLE_MAX_CONN];
        portENTER_CRITICAL(&s_conn_lock);
        for (int i = 0; i < BLE_MAX_CONN; i++) {
            conn_handles[i] = s_conn_handles[i];
            notify_enabled[i] = s_notify_enabled[i];
        }
        portEXIT_CRITICAL(&s_conn_lock);

        for (int i = 0; i < BLE_MAX_CONN; i++) {
            if (conn_handles[i] == BLE_HS_CONN_HANDLE_NONE ||
                !notify_enabled[i]) continue;
            uint16_t mtu = ble_att_mtu(conn_handles[i]);
            size_t max_payload = mtu > 3 ? mtu - 3 : 20;
            if (max_payload > MAX_NOTIFY_PAYLOAD)
                max_payload = MAX_NOTIFY_PAYLOAD;
            for (size_t offset = 0; offset < n; ) {
                size_t chunk = n - offset;
                if (chunk > max_payload) chunk = max_payload;
                bool sent = false;
                int rc = BLE_HS_ENOMEM;
                for (int retry = 0; retry < 3; retry++) {
                    struct os_mbuf *om =
                        ble_hs_mbuf_from_flat(buf + offset, chunk);
                    if (!om) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                    rc = ble_gatts_notify_custom(conn_handles[i],
                                                 s_tx_attr_handle, om);
                    if (rc == 0) {
                        sent = true;
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (!sent) {
                    ESP_LOGW(TAG, "notify failed slot=%d h=%u rc=%d",
                             i, conn_handles[i], rc);
                    break;
                }
                offset += chunk;
                vTaskDelay(1);
            }
        }
    }
}

static void ble_sink(const char *buf, size_t len, void *ctx)
{
    (void)ctx;
    bool any = false;
    portENTER_CRITICAL(&s_conn_lock);
    for (int i = 0; i < BLE_MAX_CONN; i++) {
        if (s_notify_enabled[i]) { any = true; break; }
    }
    portEXIT_CRITICAL(&s_conn_lock);
    if (!any) return;
    size_t queued = xStreamBufferSend(s_tx_sb, buf, len, 0);
    if (queued != len) {
        portENTER_CRITICAL(&s_drop_lock);
        s_tx_dropped_bytes += (uint32_t)(len - queued);
        portEXIT_CRITICAL(&s_drop_lock);
    }
}

/* ============================================================
 * Init
 * ============================================================ */
bool transport_ble_init(void)
{
    io_log("ble: init\r\n");

    portENTER_CRITICAL(&s_conn_lock);
    for (int i = 0; i < BLE_MAX_CONN; i++) {
        s_conn_handles[i]   = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled[i] = false;
        s_rx_lengths[i]     = 0;
        s_rx_discarding[i]  = false;
    }
    portEXIT_CRITICAL(&s_conn_lock);
    portENTER_CRITICAL(&s_drop_lock);
    s_tx_dropped_bytes = 0;
    portEXIT_CRITICAL(&s_drop_lock);

    s_tx_sb = xStreamBufferCreate(TX_BUF_BYTES, 1);
    if (!s_tx_sb) {
        io_log("ble: stream buffer alloc failed\r\n");
        return false;
    }

    int rc = nimble_port_init();
    io_log("ble: nimble_port_init rc=%d\r\n", rc);
    if (rc != 0) return false;

    ble_npl_callout_init(&s_adv_callout, nimble_port_get_dflt_eventq(),
                         adv_callout_fn, NULL);
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(s_svcs);
    io_log("ble: count_cfg rc=%d\r\n", rc);
    if (rc != 0) return false;

    rc = ble_gatts_add_svcs(s_svcs);
    io_log("ble: add_svcs rc=%d\r\n", rc);
    if (rc != 0) return false;

    rc = ble_svc_gap_device_name_set("Radio C5");
    if (rc != 0) {
        io_log("ble: device name setup failed rc=%d\r\n", rc);
        return false;
    }
    ble_att_set_preferred_mtu(BLE_MTU);

    TaskHandle_t tx_task_handle = NULL;
    if (xTaskCreate(tx_task, "ble_tx", 4096, NULL, 4,
                    &tx_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "failed to create TX task");
        io_log("ble: TX task creation failed\r\n");
        return false;
    }
    rc = io_register_sink(ble_sink, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to register log sink rc=%d", rc);
        io_log("ble: log sink registration failed rc=%d\r\n", rc);
        vTaskDelete(tx_task_handle);
        return false;
    }
    nimble_port_freertos_init(host_task);
    io_log("ble: host task started, waiting for sync\r\n");
    return true;
}

uint32_t transport_ble_connected_count(void)
{
    uint32_t n = 0;
    portENTER_CRITICAL(&s_conn_lock);
    for (int i = 0; i < BLE_MAX_CONN; i++) {
        if (s_conn_handles[i] != BLE_HS_CONN_HANDLE_NONE) { n++; }
    }
    portEXIT_CRITICAL(&s_conn_lock);
    return n;
}

/* ============================================================
 * Pause / Resume NUS advertising.
 *
 * While the BLE scanner runs we stop NUS advertising so the radio
 * can give its time slice to discovery (ext adv + scan on one
 * antenna otherwise fight for airtime). Active GATT connections are
 * NOT dropped — the app stays connected and keeps receiving data.
 * ============================================================ */
void transport_ble_pause_adv(void)
{
    if (s_adv_paused) return;
    s_adv_paused = true;

    if (s_nus_adv_active) {
        int rc = ble_gap_ext_adv_stop(NUS_ADV_INSTANCE);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "pause_adv: stop rc=%d", rc);
        }
        s_nus_adv_active = false;
    }
    ESP_LOGI(TAG, "NUS adv paused");
}

void transport_ble_resume_adv(void)
{
    if (!s_adv_paused) return;
    s_adv_paused = false;
    ESP_LOGI(TAG, "NUS adv resume");
    start_advertising();
}

bool transport_ble_is_adv_paused(void)
{
    return s_adv_paused;
}
#endif /* CONFIG_BT_ENABLED */
