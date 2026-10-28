/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectrum.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_err.h"
#include "esp_log.h"

#include "io.h"
#include "attack.h"
#include "monitor.h"
#include "wifi_live_scan.h"
#include "wifi_spam.h"
#include "evil_portal.h"

static const char *TAG = "spectrum";

#define SPEC_DWELL_MS   40
#define SPEC_FLOOR_RSSI (-100)

/* 2.4 GHz (1-13) + common 5 GHz channels (same set as deauth-all). */
static const uint8_t SPEC_CH_24[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const uint8_t SPEC_CH_5[]  = {
    36,40,44,48,52,56,60,64,
    100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165,
};
#define SPEC_CH_24_N (sizeof(SPEC_CH_24)/sizeof(SPEC_CH_24[0]))
#define SPEC_CH_5_N  (sizeof(SPEC_CH_5)/sizeof(SPEC_CH_5[0]))

static TaskHandle_t  s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_stop_request = false;
static bool          s_include_5ghz = false;
static int           s_cur_band = 0;   /* 0=unset, 24 or 5 — current radio band */

static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int  s_cur_peak = -128;
static volatile uint32_t s_cur_count = 0;
static volatile int64_t s_cur_rssi_sum = 0;
static volatile uint8_t s_cur_channel = 0;

static void spectrum_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA &&
        type != WIFI_PKT_CTRL) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int rssi = p->rx_ctrl.rssi;
    portENTER_CRITICAL(&s_mux);
    if (p->rx_ctrl.channel != s_cur_channel) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (rssi > s_cur_peak) s_cur_peak = rssi;
    s_cur_count++;
    s_cur_rssi_sum += rssi;
    portEXIT_CRITICAL(&s_mux);
}

static uint16_t chan_freq(uint8_t ch)
{
    if (ch <= 14) return (ch == 14) ? 2484 : (uint16_t)(2412 + (ch - 1) * 5);
    return (uint16_t)(5000 + ch * 5);
}

static void measure_channel(uint8_t ch, bool is_5ghz)
{
    /* The C5 is single-band at a time: a 5 GHz channel only actually receives
     * after the band mode is switched to 5G (same as wl_tune in
     * wifi_live_scan.c). Without this the 5 GHz channels tune-but-hear-nothing
     * and always read the floor, so only the 2.4 GHz (left) bins light up. */
    int band = is_5ghz ? 5 : 24;
    if (band != s_cur_band) {
        wifi_band_mode_t bm = is_5ghz ? WIFI_BAND_MODE_5G_ONLY
                                      : WIFI_BAND_MODE_2G_ONLY;
        if (esp_wifi_set_band_mode(bm) != ESP_OK) {
            io_log("spectrum: CH ch=%u freq=%u rssi=-128 count=0 band=%s valid=0\r\n",
                   (unsigned)ch, (unsigned)chan_freq(ch),
                   is_5ghz ? "5" : "2.4");
            return; /* band unavailable (e.g. no 5 GHz) — skip this channel */
        }
        s_cur_band = band;
    }
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        io_log("spectrum: CH ch=%u freq=%u rssi=-128 count=0 band=%s valid=0\r\n",
               (unsigned)ch, (unsigned)chan_freq(ch),
               is_5ghz ? "5" : "2.4");
        return; /* skip channels the radio can't tune right now */
    }

    portENTER_CRITICAL(&s_mux);
    s_cur_channel = ch;
    s_cur_peak = -128;
    s_cur_count = 0;
    s_cur_rssi_sum = 0;
    portEXIT_CRITICAL(&s_mux);

    vTaskDelay(pdMS_TO_TICKS(SPEC_DWELL_MS));

    portENTER_CRITICAL(&s_mux);
    int peak = s_cur_peak;
    uint32_t count = s_cur_count;
    int64_t rssi_sum = s_cur_rssi_sum;
    portEXIT_CRITICAL(&s_mux);

    int average = SPEC_FLOOR_RSSI;
    if (count > 0) {
        int64_t half = (int64_t)count / 2;
        average = (int)((rssi_sum < 0 ? rssi_sum - half : rssi_sum + half) /
                        (int64_t)count);
    }
    io_log("spectrum: CH ch=%u freq=%u rssi=%d count=%u band=%s valid=1 peak=%d\r\n",
           (unsigned)ch, (unsigned)chan_freq(ch), average, (unsigned)count,
           is_5ghz ? "5" : "2.4", count > 0 ? peak : SPEC_FLOOR_RSSI);
}

static void spectrum_task(void *arg)
{
    (void)arg;

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL,
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(spectrum_rx_cb);

    io_log("spectrum: START 5ghz=%u\r\n", (unsigned)s_include_5ghz);

    s_cur_band = 0;   /* force a band-mode set on the first channel */
    uint32_t sweep = 0;
    while (s_running && !s_stop_request) {
        for (uint32_t i = 0; i < SPEC_CH_24_N && !s_stop_request; i++) {
            measure_channel(SPEC_CH_24[i], false);
        }
        if (s_include_5ghz) {
            for (uint32_t i = 0; i < SPEC_CH_5_N && !s_stop_request; i++) {
                measure_channel(SPEC_CH_5[i], true);
            }
        }
        sweep++;
        io_log("spectrum: SWEEP n=%u\r\n", (unsigned)sweep);
    }

    /* Detach callback; keep promiscuous on (TX precondition). Restore band. */
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    io_log("spectrum: STOPPED\r\n");

    s_running = false;
    s_stop_request = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t spectrum_init(void)
{
    s_running = false;
    s_stop_request = false;
    s_task = NULL;
    return ESP_OK;
}

esp_err_t spectrum_start(bool include_5ghz)
{
    if (s_running || s_task != NULL) {
        io_log("spectrum: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Yield the WiFi radio from other exclusive users. */
    if (attack_is_running()) {
        io_log("spectrum: stopping attack first\r\n");
        attack_stop();
    }
    if (monitor_is_running()) {
        io_log("spectrum: stopping monitor first\r\n");
        monitor_stop();
    }
    if (wifi_live_scan_is_running()) {
        io_log("spectrum: stopping WiFi live scan first\r\n");
        wifi_live_scan_stop();
    }
    if (wifi_spam_is_running()) {
        io_log("spectrum: stopping WiFi spam first\r\n");
        wifi_spam_stop();
    }
    if (evil_portal_is_running()) {
        io_log("spectrum: stopping evil portal first\r\n");
        evil_portal_stop();
    }

    /* Spectrum tunes 5 GHz channels directly; keep band on AUTO. */
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);

    s_include_5ghz = include_5ghz;
    s_cur_channel = 0;
    s_stop_request = false;
    s_running = true;

    if (xTaskCreate(spectrum_task, "spectrum", 4096, NULL, 4, &s_task)
            != pdPASS) {
        s_running = false;
        s_task = NULL;
        io_log("spectrum: task create failed\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t spectrum_stop(void)
{
    if (!s_running) return ESP_OK;
    s_stop_request = true;
    s_running = false;
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool spectrum_is_running(void)
{
    return s_running;
}
