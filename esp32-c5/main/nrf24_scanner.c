/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nrf24_scanner.h"
#include "nrf24_driver.h"

#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "io.h"

static const char *TAG = "nrf_scan";

/* Each hit is a separate RX window. RPD is latched for an RX period, so
 * repeatedly reading it without ending that period would not be 32 samples. */
#define SCAN_SAMPLES_PER_CH  32
#define SCAN_DWELL_US        130
#define SCAN_CYCLE_DELAY_MS  30
#define SCAN_DUMP_FLOOR      1

/* Watchdog safety: the per-channel sampling busy-waits on the single C5 core,
 * so yield 1ms to the idle/BLE tasks roughly every 10ms of work (same pattern
 * as the jammer's jam_yield_if_needed). */
#define SCAN_YIELD_INTERVAL_US  10000
static int64_t s_last_yield_us = 0;

static inline void scan_yield_if_needed(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_yield_us >= SCAN_YIELD_INTERVAL_US) {
        s_last_yield_us = now;
        vTaskDelay(1);
    }
}

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static volatile bool     s_stop_requested = false;
static nrf24_scan_result_t s_results[NRF24_SCAN_CHANNELS];
static uint32_t          s_sweep = 0;

esp_err_t nrf24_scanner_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_results, 0, sizeof(s_results));
    s_sweep = 0;
    ESP_LOGI(TAG, "scanner init OK");
    return ESP_OK;
}

static uint8_t scan_one_channel(uint8_t ch, bool *valid)
{
    *valid = false;
    esp_err_t err = nrf24_set_channel(ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel tune failed on channel %u (%d)",
                 (unsigned)ch, (int)err);
        return 0;
    }
    esp_rom_delay_us(SCAN_DWELL_US);

    uint8_t hits = 0;
    for (int i = 0; i < SCAN_SAMPLES_PER_CH; i++) {
        bool detected = false;
        err = nrf24_sample_rpd_window(&detected);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "RPD sample failed on channel %u (%d)",
                     (unsigned)ch, (int)err);
            return 0;
        }
        if (detected) hits++;
        scan_yield_if_needed();
    }
    *valid = true;
    return hits;
}

static void waterfall_task(void *arg)
{
    (void)arg;
    /* Radio sudah di-claim oleh nrf24_scanner_waterfall_start() */
    io_log("nrfscan: waterfall start\r\n");

    esp_err_t rx_err = nrf24_enter_rx();
    if (rx_err != ESP_OK) {
        io_log("nrfscan: RX start failed (%d)\r\n", (int)rx_err);
        nrf24_standby();
        nrf24_radio_release();
        s_running = false;
        s_task = NULL;
        io_log("nrfscan: waterfall stop\r\n");
        vTaskDelete(NULL);
        return;
    }
    s_last_yield_us = esp_timer_get_time();

    while (!s_stop_requested) {
        uint8_t peak_ch = 0, peak_val = 0;
        nrf24_scan_result_t next[NRF24_SCAN_CHANNELS];
        bool sweep_valid = true;

        for (int ch = 0; ch < NRF24_SCAN_CHANNELS && !s_stop_requested; ch++) {
            bool channel_valid = false;
            uint8_t val = scan_one_channel((uint8_t)ch, &channel_valid);
            if (!channel_valid) {
                sweep_valid = false;
                break;
            }

            next[ch].channel = (uint8_t)ch;
            next[ch].rpd_count = val;
            next[ch].peak = false;

            if (val > peak_val) { peak_val = val; peak_ch = (uint8_t)ch; }
            scan_yield_if_needed();
        }

        if (s_stop_requested) break;
        if (!sweep_valid) {
            io_log("nrfscan: sweep aborted after sampling error\r\n");
            break;
        }
        if (peak_val > 0) next[peak_ch].peak = true;

        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_results, next, sizeof(s_results));
        uint32_t sweep = ++s_sweep;
        if (s_lock) xSemaphoreGive(s_lock);

        io_log("nrfscan: peak sweep=%" PRIu32 " ch=%u rpd=%u/%d\r\n",
               sweep, (unsigned)peak_ch, (unsigned)peak_val,
               SCAN_SAMPLES_PER_CH);

        vTaskDelay(pdMS_TO_TICKS(SCAN_CYCLE_DELAY_MS));
    }

    nrf24_standby();
    nrf24_radio_release();

    s_running = false;
    s_task = NULL;
    io_log("nrfscan: waterfall stop\r\n");
    vTaskDelete(NULL);
}

esp_err_t nrf24_scanner_waterfall_start(void)
{
    if (!nrf24_is_present()) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_ERR_INVALID_STATE;

    /* FIX H1: claim atomic */
    if (!nrf24_radio_claim("scanner")) {
        io_log("nrfscan: radio busy\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_results, 0, sizeof(s_results));
    s_sweep = 0;
    if (s_lock) xSemaphoreGive(s_lock);

    s_stop_requested = false;
    s_running = true;

    BaseType_t ok = xTaskCreate(waterfall_task, "nrf_scan", 4096, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        nrf24_radio_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nrf24_scanner_waterfall_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

bool nrf24_scanner_waterfall_is_running(void) { return s_running; }

int nrf24_scanner_peak(uint8_t *out_ch)
{
    if (!nrf24_is_present() || !out_ch) return -1;
    if (s_running) return -1;

    /* FIX H1: claim atomic */
    if (!nrf24_radio_claim("scan")) {
        io_log("nrfscan: radio busy\r\n");
        return -1;
    }

    esp_err_t rx_err = nrf24_enter_rx();
    if (rx_err != ESP_OK) {
        io_log("nrfscan: RX start failed (%d)\r\n", (int)rx_err);
        nrf24_standby();
        nrf24_radio_release();
        return -1;
    }
    s_last_yield_us = esp_timer_get_time();

    uint8_t best_ch = 0, best_val = 0;
    for (int ch = 0; ch < NRF24_SCAN_CHANNELS; ch++) {
        bool valid = false;
        uint8_t v = scan_one_channel((uint8_t)ch, &valid);
        if (!valid) {
            nrf24_standby();
            nrf24_radio_release();
            return -1;
        }
        if (v > best_val) { best_val = v; best_ch = (uint8_t)ch; }
        scan_yield_if_needed();
    }
    *out_ch = best_ch;

    nrf24_standby();
    nrf24_radio_release();
    return (int)best_val;
}

void nrf24_scanner_dump(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    io_log("nrfscan: LS sweep=%" PRIu32 "\r\n", s_sweep);
    for (int ch = 0; ch < NRF24_SCAN_CHANNELS; ch++) {
        /* Zero-detection channels are implicit zeroes in this complete sweep
         * snapshot. A single detection is retained rather than hidden. */
        if (s_results[ch].rpd_count >= SCAN_DUMP_FLOOR || s_results[ch].peak)
            io_log("nrfscan: CH %u rpd=%u%s\r\n",
                   (unsigned)ch,
                   (unsigned)s_results[ch].rpd_count,
                   s_results[ch].peak ? " PEAK" : "");
    }
    io_log("nrfscan: LS end\r\n");
    xSemaphoreGive(s_lock);
}

const nrf24_scan_result_t *nrf24_scanner_get_results(void) { return s_results; }
