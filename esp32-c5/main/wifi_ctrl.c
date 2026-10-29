/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "wifi_ctrl.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_log.h"

#include "io.h"
#include "targets.h"          /* TARGETS_MAX_APS didefinisikan di sini */
#include "wifi_live_scan.h"   /* wifi_live_scan_snapshot() — passive discovery */

static const char *TAG = "wifi";
static SemaphoreHandle_t s_scan_mutex;
static esp_netif_t *s_ap_netif = NULL;   /* created lazily on first AP enter */
static volatile bool s_scan_cancel_requested;
static volatile bool s_scan_async_active;

void wifi_ctrl_init(void)
{
    s_scan_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_scan_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(sta_netif ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Country code — nchan MAKSIMUM 14 (2.4 GHz).
     * 5 GHz dikontrol oleh esp_wifi_set_band_mode(). */
    wifi_country_t country = {
        .cc     = "00",
        .schan  = 1,
        .nchan  = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_err_t cc_err = esp_wifi_set_country(&country);
    if (cc_err != ESP_OK) {
        ESP_LOGW(TAG, "set_country failed: %d", cc_err);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Aktifkan dual-band (2.4 + 5 GHz) — wajib untuk C5 */
    esp_err_t bm_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (bm_err != ESP_OK) {
        ESP_LOGW(TAG, "set_band_mode(AUTO) failed: %d", bm_err);
    }

    /* Promiscuous mode — precondition untuk esp_wifi_80211_tx() */
    esp_err_t pm_err = esp_wifi_set_promiscuous(true);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "set_promiscuous(true) failed: %d", pm_err);
    }

    ESP_LOGI(TAG, "wifi: STA up, country=00, band=AUTO, promiscuous on");
}

static void clear_scan_targets(void)
{
    targets_set_aps(NULL, 0);
    targets_sel_clear();
    targets_clear_selection();
}

/* Network Target discovery. Driven entirely by the passive live-scan engine:
 * it hops every 2.4 + 5 GHz channel in promiscuous mode and records the
 * beacons it hears, then mirrors the result straight into the shared targets
 * table. Unlike the old active esp_wifi_scan_start path it reliably sees 5 GHz
 * APs under BLE coexistence (esp-idf#16059 / IDFGH-15407), and it emits no
 * wlscan telemetry, so the `scan` wire protocol (scan: start / scan: N APs)
 * and the monitor/select/deauth flow that reads the target table are all
 * unchanged. */
static int wifi_ctrl_scan_internal(void)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (s_scan_cancel_requested) {
        xSemaphoreGive(s_scan_mutex);
        return -2;
    }
    io_log("scan: start (2.4 + 5 GHz)\r\n");

    /* One bounded dual-band sweep, ~150 ms listen per channel. The snapshot
     * writes the captured APs into the target table itself. */
    int total = wifi_live_scan_snapshot(true, 1, 150, &s_scan_cancel_requested);

    if (total == -2) {
        clear_scan_targets();
        xSemaphoreGive(s_scan_mutex);
        return -2;
    }
    if (total == -3) {
        /* Radio owned by the continuous WiFi live scan — not a failure. */
        io_log("scan: busy (live scan running)\r\n");
        xSemaphoreGive(s_scan_mutex);
        return -3;
    }
    if (total < 0) {
        io_log("scan: failed\r\n");
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }

    targets_clear_selection();
    io_log("scan: %d APs\r\n", total);
    xSemaphoreGive(s_scan_mutex);
    return total;
}

int wifi_ctrl_scan(void)
{
    s_scan_cancel_requested = false;
    return wifi_ctrl_scan_internal();
}

static void wifi_ctrl_scan_task(void *arg)
{
    (void)arg;
    int result = wifi_ctrl_scan_internal();
    if (result == -2) io_log("scan: stopped\r\n");
    s_scan_async_active = false;
    vTaskDelete(NULL);
}

esp_err_t wifi_ctrl_scan_start(void)
{
    if (s_scan_async_active) return ESP_ERR_INVALID_STATE;
    s_scan_cancel_requested = false;
    s_scan_async_active = true;
    if (xTaskCreate(wifi_ctrl_scan_task, "wifi_scan", 4096, NULL, 5, NULL) != pdPASS) {
        s_scan_async_active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_ctrl_scan_stop(void)
{
    if (!s_scan_async_active) return ESP_ERR_INVALID_STATE;
    s_scan_cancel_requested = true;
    for (int i = 0; i < 500 && s_scan_async_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_scan_async_active ? ESP_ERR_TIMEOUT : ESP_OK;
}

bool wifi_ctrl_scan_is_running(void)
{
    return s_scan_async_active;
}

void wifi_ctrl_scan_clear(void)
{
    clear_scan_targets();
    io_log("scan: cleared\r\n");
}

void wifi_ctrl_set_channel(uint8_t channel)
{
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

/* Make the SoftAP's DHCP server hand clients the AP itself as their DNS
 * resolver, so a captive-portal DNS hijack can answer every lookup. Must run
 * while the DHCP server is stopped — which it is right after the AP netif is
 * created and before the AP interface comes up. */
static void ap_set_captive_dns(esp_netif_t *netif)
{
    esp_netif_dns_info_t dns = {
        .ip.type = ESP_IPADDR_TYPE_V4,
        .ip.u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1),
    };
    esp_netif_dhcps_stop(netif);
    uint8_t offer_dns = 0x02;   /* OFFER_DNS: hand clients a DNS server */
    esp_err_t e1 = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                          ESP_NETIF_DOMAIN_NAME_SERVER,
                                          &offer_dns, sizeof(offer_dns));
    esp_err_t e2 = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "ap dhcps dns option set failed: %d/%d", e1, e2);
    }

    /* RFC 8910 (DHCP option 114): advertise the portal URL directly so
     * Android 11+/iOS 14+ surface the sign-in page reliably instead of
     * relying only on the connectivity-probe redirect. Best-effort — a
     * client or build without support just falls back to the probe path.
     * NOTE: this option stores the POINTER (not a copy — see esp_netif.h),
     * so the buffer must outlive the dhcps; it MUST be static, never a
     * stack local. */
    static char uri[] = "http://192.168.4.1/";
    esp_err_t e3 = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                          ESP_NETIF_CAPTIVEPORTAL_URI,
                                          uri, strlen(uri));
    if (e3 != ESP_OK) {
        ESP_LOGW(TAG, "ap captive-portal URI option set failed: %d", e3);
    }
    /* dhcps restarts automatically on the AP-start event. */
}

esp_err_t wifi_ctrl_ap_enter(const char *ssid, uint8_t channel)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    if (channel < 1 || channel > 13) channel = 1;

    /* Promiscuous and AP service cannot share the radio. */
    esp_wifi_set_promiscuous(false);

    /* A SoftAP needs its own netif (+ default DHCP server handlers). Create it
     * once; it coexists with the STA netif made at init. */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            esp_wifi_set_promiscuous(true);
            return ESP_ERR_NO_MEM;
        }
        ap_set_captive_dns(s_ap_netif);
    }

    wifi_config_t ap = { 0 };
    size_t len = strlen(ssid);
    if (len > 32) len = 32;
    memcpy(ap.ap.ssid, ssid, len);
    ap.ap.ssid_len       = (uint8_t)len;
    ap.ap.channel        = channel;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 8;
    ap.ap.beacon_interval = 100;

    /* Captive portals live on 2.4 GHz — the band every client supports. */
    esp_err_t be = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (be != ESP_OK) ESP_LOGW(TAG, "ap set_band_mode(2G) failed: %d", be);

    esp_err_t me = esp_wifi_set_mode(WIFI_MODE_AP);
    if (me != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(AP) failed: %d", me);
        wifi_ctrl_ap_exit();
        return me;
    }
    esp_err_t ce = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (ce != ESP_OK) {
        ESP_LOGE(TAG, "set_config(AP) failed: %d", ce);
        wifi_ctrl_ap_exit();
        return ce;
    }

    ESP_LOGI(TAG, "softap up ssid='%s' ch=%u ip=192.168.4.1",
             ssid, (unsigned)channel);
    return ESP_OK;
}

esp_err_t wifi_ctrl_ap_exit(void)
{
    /* Restore the STA + promiscuous + AUTO-band baseline the rest of the
     * firmware assumes. The AP netif is left allocated for reuse. */
    esp_err_t me = esp_wifi_set_mode(WIFI_MODE_STA);
    if (me != ESP_OK) ESP_LOGW(TAG, "set_mode(STA) restore failed: %d", me);

    esp_err_t be = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (be != ESP_OK) ESP_LOGW(TAG, "set_band_mode(AUTO) restore failed: %d", be);

    esp_err_t pe = esp_wifi_set_promiscuous(true);
    if (pe != ESP_OK) ESP_LOGW(TAG, "set_promiscuous restore failed: %d", pe);

    return ESP_OK;
}
