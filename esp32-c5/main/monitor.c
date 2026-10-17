#include "monitor.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"

#include "io.h"
#include "targets.h"
#include "attack.h"
#include "spectrum.h"
#include "wifi_live_scan.h"
#include "wifi_spam.h"
#include "evil_portal.h"

static const char *TAG = "monitor";

/* Window between STAT reports (ms). FPS is computed over this window. */
#define MONITOR_REPORT_MS   1000
#define MONITOR_MAX_STAS    32

/* ============================================================
 * State
 * ============================================================ */
static TaskHandle_t  s_task          = NULL;
static volatile bool s_running       = false;
static volatile bool s_stop_request  = false;

static uint8_t       s_bssid[6];
static uint8_t       s_channel;
static bool          s_band_5ghz;

static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

/* counters guarded by s_mux */
static uint32_t      s_frames_total;   /* lifetime */
static uint32_t      s_mgmt_total;
static uint32_t      s_data_total;
static uint32_t      s_window_frames;  /* reset every report window */
static int8_t        s_last_rssi;
static uint8_t       s_stas[MONITOR_MAX_STAS][6];
static int           s_sta_count;

static void monitor_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type);

/* ============================================================
 * Helpers
 * ============================================================ */
static void reset_counters(void)
{
    portENTER_CRITICAL(&s_mux);
    s_frames_total  = 0;
    s_mgmt_total    = 0;
    s_data_total    = 0;
    s_window_frames = 0;
    s_last_rssi     = 0;
    s_sta_count     = 0;
    portEXIT_CRITICAL(&s_mux);
}

/* Add a STA MAC to the dedup table. Caller holds s_mux. */
static void note_sta_locked(const uint8_t *mac)
{
    for (int i = 0; i < s_sta_count; i++) {
        if (memcmp(s_stas[i], mac, 6) == 0) return;
    }
    if (s_sta_count < MONITOR_MAX_STAS) {
        memcpy(s_stas[s_sta_count], mac, 6);
        s_sta_count++;
    }
}

/* ============================================================
 * Promiscuous RX callback
 * ============================================================ */
static void monitor_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p->rx_ctrl.sig_len < 24) return;
    const uint8_t *f = p->payload;

    uint8_t fc0 = f[0];
    uint8_t fc1 = f[1];
    uint8_t ftype = (fc0 >> 2) & 0x3;
    bool toDS   = fc1 & 0x01;
    bool fromDS = fc1 & 0x02;

    const uint8_t *a1 = f + 4;
    const uint8_t *a2 = f + 10;
    const uint8_t *a3 = f + 16;

    const uint8_t *bssid = NULL;
    const uint8_t *sta   = NULL;

    if (ftype == 0) {            /* mgmt: BSSID = addr3, STA = addr2 */
        bssid = a3;
        sta   = a2;
    } else if (ftype == 2) {     /* data */
        if (toDS && !fromDS)      { bssid = a1; sta = a2; }
        else if (!toDS && fromDS) { bssid = a2; sta = a1; }
        else                      { return; }
    } else {
        return;
    }

    if (memcmp(bssid, s_bssid, 6) != 0) return;

    portENTER_CRITICAL(&s_mux);
    s_frames_total++;
    s_window_frames++;
    s_last_rssi = p->rx_ctrl.rssi;
    if (ftype == 0) s_mgmt_total++;
    else            s_data_total++;
    /* track associated STAs (skip multicast + the AP mirrored back) */
    if (sta && !(sta[0] & 0x01) && memcmp(sta, bssid, 6) != 0) {
        note_sta_locked(sta);
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ============================================================
 * Monitor task
 * ============================================================ */
/* Restore radio state and mark the monitor as finished. */
static void monitor_finish(void)
{
    /* Detach our RX callback; leave promiscuous mode enabled because the
     * TX path (esp_wifi_80211_tx) requires it. Restore automatic band. */
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);

    io_log("monitor: STOPPED\r\n");

    s_running = false;
    s_stop_request = false;
    s_task = NULL;
}

static void monitor_task(void *arg)
{
    (void)arg;

    /* Select the band that matches the target channel, then lock to it. */
    esp_err_t berr = esp_wifi_set_band_mode(
        s_band_5ghz ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY);
    if (berr != ESP_OK) {
        ESP_LOGW(TAG, "set_band_mode rc=%d", berr);
    }

    esp_err_t cerr = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    if (cerr != ESP_OK) {
        io_log("monitor: cannot tune ch=%u (err=%d)\r\n",
               (unsigned)s_channel, (int)cerr);
        monitor_finish();
        vTaskDelete(NULL);
        return;
    }

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(monitor_rx_cb);

    io_log("monitor: START ch=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X band=%s\r\n",
           (unsigned)s_channel,
           s_bssid[0], s_bssid[1], s_bssid[2],
           s_bssid[3], s_bssid[4], s_bssid[5],
           s_band_5ghz ? "5" : "2.4");

    int64_t next_report = esp_timer_get_time() + MONITOR_REPORT_MS * 1000;

    while (s_running && !s_stop_request) {
        vTaskDelay(pdMS_TO_TICKS(50));

        int64_t now = esp_timer_get_time();
        if (now < next_report) continue;
        next_report = now + MONITOR_REPORT_MS * 1000;

        portENTER_CRITICAL(&s_mux);
        uint32_t win   = s_window_frames;
        uint32_t total = s_frames_total;
        uint32_t mgmt  = s_mgmt_total;
        uint32_t data  = s_data_total;
        int8_t   rssi  = s_last_rssi;
        int      stas  = s_sta_count;
        s_window_frames = 0;
        portEXIT_CRITICAL(&s_mux);

        uint32_t fps = (win * 1000u) / MONITOR_REPORT_MS;

        io_log("monitor: STAT rssi=%d fps=%" PRIu32 " frames=%" PRIu32
               " mgmt=%" PRIu32 " data=%" PRIu32 " stas=%d\r\n",
               (int)rssi, fps, total, mgmt, data, stas);
    }

    monitor_finish();
    vTaskDelete(NULL);
}

/* ============================================================
 * Public API
 * ============================================================ */
esp_err_t monitor_init(void)
{
    s_running      = false;
    s_stop_request = false;
    s_task         = NULL;
    return ESP_OK;
}

esp_err_t monitor_start(uint16_t ap_idx)
{
    if (s_running || s_task != NULL) {
        io_log("monitor: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (attack_is_running()) {
        io_log("monitor: stop the active attack first\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (spectrum_is_running()) {
        io_log("monitor: stopping spectrum first\r\n");
        spectrum_stop();
    }
    if (wifi_live_scan_is_running()) {
        io_log("monitor: stopping WiFi live scan first\r\n");
        wifi_live_scan_stop();
    }
    if (wifi_spam_is_running()) {
        io_log("monitor: stopping WiFi spam first\r\n");
        wifi_spam_stop();
    }
    if (evil_portal_is_running()) {
        io_log("monitor: stopping evil portal first\r\n");
        evil_portal_stop();
    }

    target_ap_t ap;
    if (targets_get_ap(ap_idx, &ap) != 0) {
        io_log("monitor: bad ap_idx\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_bssid, ap.bssid, 6);
    s_channel   = ap.channel;
    s_band_5ghz = ap.band_5ghz;
    reset_counters();
    s_stop_request = false;
    s_running      = true;

    if (xTaskCreate(monitor_task, "monitor", 4096, NULL, 4, &s_task)
            != pdPASS) {
        s_running = false;
        s_task    = NULL;
        io_log("monitor: task create failed\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t monitor_stop(void)
{
    if (!s_running) return ESP_OK;
    s_stop_request = true;
    s_running      = false;
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool monitor_is_running(void)
{
    return s_running;
}
