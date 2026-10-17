#include "ble_spectrum.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include "io.h"
#include "transport_ble.h"
#include "ble_spam.h"
#include "ble_scanner.h"
#include "wifi_spam.h"

static const char *TAG = "ble_spectrum";

/* Shorter window → the app receives a FRAME ~6-7× per second instead of ~2.5×,
 * so the waterfall scrolls fast enough to separate individual advertising
 * bursts and the live strip tracks changes with far less delay. Still long
 * enough to collect a representative RSSI sample across the 3 adv channels. */
#define WINDOW_MS 150
#define BLE_SPECTRUM_RSSI_UNAVAILABLE 127

static TaskHandle_t  s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_stop_request = false;
static uint8_t       s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool          s_addr_ready = false;

static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      s_bins[BLE_SPECTRUM_BINS];
static int           s_peak = -128;
static uint32_t      s_total = 0;
static int64_t       s_rssi_sum = 0;

static bool ensure_addr(void)
{
    if (s_addr_ready) return true;
    if (ble_hs_util_ensure_addr(0) != 0) return false;
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) return false;
    s_addr_ready = true;
    return true;
}

static inline int rssi_bin(int rssi)
{
    int b = (rssi + 100) / 6; /* -100 -> 0 ... -40 -> 10 */
    if (b < 0) b = 0;
    if (b >= BLE_SPECTRUM_BINS) b = BLE_SPECTRUM_BINS - 1;
    return b;
}

static void record_rssi(int rssi)
{
    if (rssi == BLE_SPECTRUM_RSSI_UNAVAILABLE) return;

    portENTER_CRITICAL(&s_mux);
    s_bins[rssi_bin(rssi)]++;
    if (rssi > s_peak) s_peak = rssi;
    s_total++;
    s_rssi_sum += rssi;
    portEXIT_CRITICAL(&s_mux);
}

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC:
        record_rssi(event->ext_disc.rssi);
        break;
#else
    case BLE_GAP_EVENT_DISC:
        record_rssi(event->disc.rssi);
        break;
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_running = false;
        break;
    default:
        break;
    }
    return 0;
}

static void spectrum_task(void *arg)
{
    (void)arg;
    io_log("blespectrum: START\r\n");

    while (s_running && !s_stop_request) {
        vTaskDelay(pdMS_TO_TICKS(WINDOW_MS));

        uint32_t bins[BLE_SPECTRUM_BINS];
        int peak;
        uint32_t total;
        int64_t rssi_sum;
        portENTER_CRITICAL(&s_mux);
        memcpy(bins, s_bins, sizeof(bins));
        peak = s_peak;
        total = s_total;
        rssi_sum = s_rssi_sum;
        memset(s_bins, 0, sizeof(s_bins));
        s_peak = -128;
        s_total = 0;
        s_rssi_sum = 0;
        portEXIT_CRITICAL(&s_mux);

        int64_t half = (int64_t)total / 2;
        int mean = total > 0
            ? (int)((rssi_sum < 0 ? rssi_sum - half : rssi_sum + half) /
                    (int64_t)total)
            : -100;
        io_log("blespectrum: FRAME mean=%d peak=%d total=%" PRIu32
               " bins=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
               ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
               ",%" PRIu32 ",%" PRIu32 "\r\n",
               mean, (total > 0) ? peak : -100, total,
               bins[0], bins[1], bins[2], bins[3], bins[4],
               bins[5], bins[6], bins[7], bins[8], bins[9]);
    }

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "disc_cancel rc=%d", rc);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    transport_ble_resume_adv();

    io_log("blespectrum: STOPPED\r\n");
    s_running = false;
    s_stop_request = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ble_spectrum_init(void)
{
    s_running = false;
    s_stop_request = false;
    s_task = NULL;
    return ESP_OK;
}

esp_err_t ble_spectrum_start(void)
{
    if (s_running || s_task != NULL) {
        io_log("blespectrum: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Yield the BLE radio from other exclusive users. */
    if (ble_spam_is_running()) {
        io_log("blespectrum: stopping BLE spam first\r\n");
        ble_spam_stop();
    }
    if (ble_scanner_is_running()) {
        io_log("blespectrum: stopping BLE scan first\r\n");
        ble_scanner_stop();
    }
    if (wifi_spam_is_running()) {
        io_log("blespectrum: stopping WiFi spam first\r\n");
        wifi_spam_stop();
    }

    if (!ensure_addr()) {
        io_log("blespectrum: BLE not synced yet\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_mux);
    memset(s_bins, 0, sizeof(s_bins));
    s_peak = -128;
    s_total = 0;
    s_rssi_sum = 0;
    portEXIT_CRITICAL(&s_mux);

    transport_ble_pause_adv();
    vTaskDelay(pdMS_TO_TICKS(50));

    struct ble_gap_disc_params params = {0};
    params.passive          = 1; /* passive: just sample RSSI/activity */
    params.filter_duplicates = 0;
    params.itvl             = 0x0010;
    params.window           = 0x0010;
    params.filter_policy    = 0;
    params.limited          = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                          scan_event_cb, NULL);
    if (rc != 0) {
        io_log("blespectrum: ble_gap_disc rc=%d\r\n", rc);
        transport_ble_resume_adv();
        return ESP_FAIL;
    }

    s_stop_request = false;
    s_running = true;

    if (xTaskCreate(spectrum_task, "ble_spec", 4096, NULL, 3, &s_task)
            != pdPASS) {
        s_running = false;
        s_task = NULL;
        ble_gap_disc_cancel();
        transport_ble_resume_adv();
        io_log("blespectrum: task create failed\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ble_spectrum_stop(void)
{
    if (!s_running && s_task == NULL) return ESP_OK;
    s_stop_request = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task == NULL && s_running) {
        s_running = false;
        ble_gap_disc_cancel();
        transport_ble_resume_adv();
    }
    return ESP_OK;
}

bool ble_spectrum_is_running(void)
{
    return s_running;
}
