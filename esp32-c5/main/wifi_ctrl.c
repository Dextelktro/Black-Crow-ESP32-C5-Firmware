#include "wifi_ctrl.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "io.h"
#include "targets.h"      /* TARGETS_MAX_APS didefinisikan di sini */

static const char *TAG = "wifi";
static volatile bool s_scan_done = false;
static SemaphoreHandle_t s_scan_mutex;
static SemaphoreHandle_t s_scan_done_sem;
static esp_netif_t *s_ap_netif = NULL;   /* created lazily on first AP enter */
static volatile bool s_scan_cancel_requested;
static volatile bool s_scan_async_active;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_scan_done = true;
        if (s_scan_done_sem) xSemaphoreGive(s_scan_done_sem);
    }
}

void wifi_ctrl_init(void)
{
    s_scan_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_scan_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    s_scan_done_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_scan_done_sem ? ESP_OK : ESP_ERR_NO_MEM);

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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                                WIFI_EVENT_SCAN_DONE,
                                                &wifi_event_handler, NULL));

    ESP_LOGI(TAG, "wifi: STA up, country=00, band=AUTO, promiscuous on");
}

static void clear_scan_targets(void)
{
    targets_set_aps(NULL, 0);
    targets_sel_clear();
    targets_clear_selection();
}

/* Scan satu band dan kumpulkan hasil. Return jumlah, atau -1 jika timeout. */
static int do_scan_and_collect(target_ap_t *out_aps, int max_aps)
{
    s_scan_done = false;
    while (xSemaphoreTake(s_scan_done_sem, 0) == pdTRUE) {}

    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0, .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        /* Keep the full dwell on every channel to avoid missing weak APs. */
        .scan_time.active = { .min = 120, .max = 120 },
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) return -1;

    /* Sleep on the scan-done event instead of polling every 100 ms. */
    for (int i = 0; i < 500 && !s_scan_done; i++) {
        if (s_scan_cancel_requested) {
            esp_err_t stop_err = esp_wifi_scan_stop();
            if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "scan_stop after request failed: %d", stop_err);
            }
            xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS(150));
            s_scan_done = false;
            while (xSemaphoreTake(s_scan_done_sem, 0) == pdTRUE) {}
            return -2;
        }
        xSemaphoreTake(s_scan_done_sem, pdMS_TO_TICKS(20));
    }
    if (!s_scan_done) {
        esp_err_t stop_err = esp_wifi_scan_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "scan_stop after timeout failed: %d", stop_err);
        }
        /* esp_wifi_scan_stop() still fires a (delayed) WIFI_EVENT_SCAN_DONE.
         * Drain it so a stale event can't flip s_scan_done true for the NEXT
         * scan and make it return 0 APs. */
        vTaskDelay(pdMS_TO_TICKS(150));
        s_scan_done = false;
        while (xSemaphoreTake(s_scan_done_sem, 0) == pdTRUE) {}
        return -1;
    }

    uint16_t n = 0;
    err = esp_wifi_scan_get_ap_num(&n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_num failed: %d", err);
        return -1;
    }
    if (n == 0) return 0;
    if (n > max_aps) n = max_aps;

    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return -1;

    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) { free(recs); return -1; }

    int count = 0;
    for (uint16_t i = 0; i < n; i++) {
        memcpy(out_aps[count].bssid, recs[i].bssid, 6);
        strncpy(out_aps[count].ssid, (const char *)recs[i].ssid,
                sizeof(out_aps[count].ssid) - 1);
        out_aps[count].ssid[sizeof(out_aps[count].ssid) - 1] = '\0';
        out_aps[count].channel = recs[i].primary;
        out_aps[count].rssi = recs[i].rssi;
        out_aps[count].band_5ghz = (recs[i].primary > 14);
        count++;
    }
    free(recs);
    return count;
}

static int wifi_ctrl_scan_internal(void)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    if (s_scan_cancel_requested) {
        xSemaphoreGive(s_scan_mutex);
        return -2;
    }
    io_log("scan: start (2.4 GHz)\r\n");

    target_ap_t *aps = heap_caps_calloc(TARGETS_MAX_APS, sizeof(*aps),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aps) {
        io_log("scan: failed (out of memory)\r\n");
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }

    /* LAYER 1: Scan 2.4 GHz — selalu berhasil meski BLE aktif */
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (band_err != ESP_OK) {
        ESP_LOGE(TAG, "set_band_mode(2G_ONLY) failed: %d", band_err);
        io_log("scan: failed (cannot select 2.4 GHz band)\r\n");
        heap_caps_free(aps);
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }
    int n24 = do_scan_and_collect(aps, TARGETS_MAX_APS);
    if (n24 < 0) {
        esp_err_t restore_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "set_band_mode(AUTO) after scan failure failed: %d",
                     restore_err);
        }
        if (n24 == -2) {
            clear_scan_targets();
            heap_caps_free(aps);
            xSemaphoreGive(s_scan_mutex);
            return -2;
        }
        io_log("scan: failed (2.4 GHz scan)\r\n");
        heap_caps_free(aps);
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }
    int total = n24;

    /* LAYER 2: Scan 5 GHz — bisa timeout jika BLE aktif (IDFGH-15407) */
    if (s_scan_cancel_requested) {
        band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (band_err != ESP_OK) {
            ESP_LOGE(TAG, "set_band_mode(AUTO) after scan stop failed: %d",
                     band_err);
        }
        clear_scan_targets();
        heap_caps_free(aps);
        xSemaphoreGive(s_scan_mutex);
        return -2;
    }

    if (total < TARGETS_MAX_APS) {
        io_log("scan: 5 GHz...\r\n");
        band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
        if (band_err != ESP_OK) {
            ESP_LOGW(TAG, "set_band_mode(5G_ONLY) failed: %d", band_err);
            io_log("scan: 5 GHz unavailable\r\n");
        } else {
            int remaining = TARGETS_MAX_APS - total;
            int n5 = do_scan_and_collect(&aps[total], remaining);
            if (n5 == -2) {
                esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
                clear_scan_targets();
                heap_caps_free(aps);
                xSemaphoreGive(s_scan_mutex);
                return -2;
            }
            if (n5 > 0) {
                total += n5;
            } else if (n5 < 0) {
                io_log("scan: 5 GHz scan failed (BLE active?)\r\n");
            }
        }
    }

    band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (band_err != ESP_OK) {
        ESP_LOGE(TAG, "set_band_mode(AUTO) after scan failed: %d", band_err);
        io_log("scan: failed (cannot restore automatic band selection)\r\n");
        heap_caps_free(aps);
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }

    targets_set_aps(aps, total);
    targets_clear_selection();
    heap_caps_free(aps);

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

/* Fast active scan on the CURRENT band. Starts the scan, waits (checking the
 * stop flag), and appends parsed results to out_aps starting at `offset`.
 * Returns the number appended, or -1 on start failure / abort. */
static int fast_scan_current_band(target_ap_t *out_aps, int offset, int max_aps,
                                  uint32_t min_ms, uint32_t max_ms,
                                  int timeout_iter, volatile bool *stop_flag)
{
    if (offset >= max_aps) return 0;
    if (stop_flag && *stop_flag) return -1;

    s_scan_done = false;
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL,
        .channel = 0, .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = min_ms, .max = max_ms },
    };
    if (esp_wifi_scan_start(&cfg, false) != ESP_OK) return -1;

    for (int i = 0; i < timeout_iter && !s_scan_done; i++) {
        if (stop_flag && *stop_flag) {
            esp_wifi_scan_stop();
            vTaskDelay(pdMS_TO_TICKS(150));  /* drain pending SCAN_DONE */
            s_scan_done = false;
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (!s_scan_done) {
        esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(150));      /* drain pending SCAN_DONE */
        s_scan_done = false;
        return -1;
    }

    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK || n == 0) return 0;
    int room = max_aps - offset;
    if (n > room) n = room;

    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        return 0;
    }

    int count = 0;
    for (uint16_t i = 0; i < n; i++) {
        target_ap_t *a = &out_aps[offset + count];
        memcpy(a->bssid, recs[i].bssid, 6);
        strncpy(a->ssid, (const char *)recs[i].ssid, sizeof(a->ssid) - 1);
        a->ssid[sizeof(a->ssid) - 1] = '\0';
        a->channel = recs[i].primary;
        a->rssi = recs[i].rssi;
        a->band_5ghz = (recs[i].primary > 14);
        count++;
    }
    free(recs);
    return count;
}

int wifi_ctrl_scan_fast(bool band_5g, uint32_t min_ms, uint32_t max_ms,
                        volatile bool *stop_flag)
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    target_ap_t *aps = heap_caps_calloc(TARGETS_MAX_APS, sizeof(*aps),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!aps) {
        xSemaphoreGive(s_scan_mutex);
        return -1;
    }

    int total = 0;
    int ret = -1; /* -1 = fail, >=0 = ok */

    if (stop_flag && *stop_flag) goto done;

    /* 2.4 GHz (always, ~5s timeout) */
    if (esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY) == ESP_OK) {
        int n24 = fast_scan_current_band(aps, 0, TARGETS_MAX_APS,
                                         min_ms, max_ms, 200, stop_flag);
        if (n24 >= 0) {
            total = n24;
            ret = 0; /* at least 2.4 GHz succeeded */
        }
    }

    /* 5 GHz (optional, ~7.5s timeout) */
    if (ret == 0 && band_5g && total < TARGETS_MAX_APS &&
        !(stop_flag && *stop_flag)) {
        if (esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY) == ESP_OK) {
            int n5 = fast_scan_current_band(aps, total, TARGETS_MAX_APS,
                                            min_ms, max_ms, 300, stop_flag);
            if (n5 > 0) total += n5;
        }
    }

done:
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (ret == 0) targets_set_aps(aps, total);
    heap_caps_free(aps);
    xSemaphoreGive(s_scan_mutex);
    return (ret == 0) ? total : -1;
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
