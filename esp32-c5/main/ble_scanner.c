#include "ble_scanner.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include "io.h"
#include "transport_ble.h"
#include "ble_spam.h"
#include "ble_spectrum.h"
#include "wifi_spam.h"

static const char *TAG = "ble_scanner";

/* ============================================================
 * State
 * ============================================================ */
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_progress_task = NULL;
static volatile bool     s_running = false;
static volatile bool     s_stop_requested = false;
static uint32_t          s_duration_sec = 0;
static int64_t           s_started_us = 0;
static uint8_t           s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool              s_addr_ready = false;

/* Buffer devices */
static ble_scan_entry_t  s_devices[BLE_SCAN_MAX_DEVICES];
static uint32_t          s_device_count = 0;

/* ============================================================
 * Helpers
 * ============================================================ */
static bool ensure_addr(void)
{
    if (s_addr_ready) return true;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return false;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return false;
    }
    s_addr_ready = true;
    return true;
}

/* Parse AD structure untuk ekstrak nama dan manufacturer ID */
static void parse_ad_data(const uint8_t *data, uint8_t len,
                          char *name_out, size_t name_max,
                          uint16_t *mfg_id_out)
{
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t field_len = data[i];
        if (field_len == 0) break;
        if (i + 1 + field_len > len) break;

        uint8_t field_type = data[i + 1];
        const uint8_t *field_val = data + i + 2;
        uint8_t val_len = field_len - 1;

        switch (field_type) {
            case 0x08:  /* Shortened Local Name */
            case 0x09:  /* Complete Local Name */
                if (val_len > 0 && val_len < name_max) {
                    memcpy(name_out, field_val, val_len);
                    name_out[val_len] = '\0';
                }
                break;
            case 0xFF:  /* Manufacturer Specific Data */
                if (val_len >= 2) {
                    *mfg_id_out = (uint16_t)field_val[0] |
                                  ((uint16_t)field_val[1] << 8);
                }
                break;
            default:
                break;
        }
        i += 1 + field_len;
    }
}

/* Cari entry berdasarkan MAC, return index atau -1 */
static int find_device(const uint8_t *addr, uint8_t addr_type)
{
    for (uint32_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].addr_type == addr_type &&
            memcmp(s_devices[i].addr, addr, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Tambah atau update device di buffer. Return true jika entry BARU. */
static bool add_or_update_device(const uint8_t *addr, uint8_t addr_type,
                                  int8_t rssi, uint8_t adv_type,
                                  const uint8_t *adv_data, uint8_t adv_len)
{
    int idx = find_device(addr, addr_type);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    char name[BLE_SCAN_NAME_LEN] = {0};
    uint16_t mfg_id = 0xFFFF;
    parse_ad_data(adv_data, adv_len, name, sizeof(name), &mfg_id);

    if (idx >= 0) {
        /* Update entry lama */
        s_devices[idx].rssi = rssi;
        s_devices[idx].seen++;
        s_devices[idx].last_seen_ms = now_ms;
        if (name[0] != '\0' && s_devices[idx].name_len == 0) {
            strncpy(s_devices[idx].name, name, BLE_SCAN_NAME_LEN - 1);
            s_devices[idx].name[BLE_SCAN_NAME_LEN - 1] = '\0';
            s_devices[idx].name_len = (uint8_t)strlen(s_devices[idx].name);
        }
        if (mfg_id != 0xFFFF && s_devices[idx].mfg_id == 0xFFFF) {
            s_devices[idx].mfg_id = mfg_id;
        }
        return false;
    }

    /* Buffer penuh */
    if (s_device_count >= BLE_SCAN_MAX_DEVICES) return false;

    /* Entry baru */
    ble_scan_entry_t *e = &s_devices[s_device_count];
    memcpy(e->addr, addr, 6);
    e->addr_type = addr_type;
    e->rssi = rssi;
    e->adv_type = adv_type;
    e->mfg_id = mfg_id;
    e->name_len = (uint8_t)strlen(name);
    if (e->name_len > 0) {
        strncpy(e->name, name, BLE_SCAN_NAME_LEN - 1);
        e->name[BLE_SCAN_NAME_LEN - 1] = '\0';
    } else {
        e->name[0] = '\0';
    }
    e->seen = 1;
    e->last_seen_ms = now_ms;
    s_device_count++;
    return true;
}

/* Report device ke io_log */
static void report_device(const ble_scan_entry_t *e)
{
    char mfg_str[8];
    if (e->mfg_id == 0xFFFF) {
        snprintf(mfg_str, sizeof(mfg_str), "----");
    } else {
        snprintf(mfg_str, sizeof(mfg_str), "%04X", e->mfg_id);
    }

    io_log("blescan: DEV addr=%02X:%02X:%02X:%02X:%02X:%02X "
           "rssi=%d type=%u atype=%u mfg=%s name=\"%s\"\r\n",
           e->addr[0], e->addr[1], e->addr[2],
           e->addr[3], e->addr[4], e->addr[5],
           (int)e->rssi, e->adv_type, e->addr_type, mfg_str, e->name);
}

/* Common handler untuk sebuah advertising report */
static void handle_report(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                          uint8_t adv_type, const uint8_t *data, uint8_t len)
{
    if (len == 0) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;  /* skip frame ini kalau lock sibuk */
    }
    bool is_new = add_or_update_device(addr, addr_type, rssi, adv_type,
                                       data, len);
    /* Report HANYA device baru (biar log tidak spam) */
    if (is_new) {
        report_device(&s_devices[s_device_count - 1]);
    }
    xSemaphoreGive(s_lock);
}

/* ============================================================
 * Scan event callback — dipanggil dari host task.
 *
 * Dengan CONFIG_BT_NIMBLE_EXT_ADV=y (wajib untuk BLE spam + NUS
 * extended advertising), NimBLE mengirim hasil discovery sebagai
 * BLE_GAP_EVENT_EXT_DISC (event->ext_disc), BUKAN BLE_GAP_EVENT_DISC.
 * Menangani event yang salah = nol device terdeteksi.
 * ============================================================ */
static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
        handle_report(d->addr.val, d->addr.type, d->rssi,
                      d->legacy_event_type, d->data, d->length_data);
        break;
    }
#else
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        handle_report(d->addr.val, d->addr.type, d->rssi,
                      d->event_type, d->data, d->length_data);
        break;
    }
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "disc complete reason=%d",
                 event->disc_complete.reason);
        s_running = false;
        break;

    default:
        break;
    }
    return 0;
}

/* ============================================================
 * Progress task — lapor hasil tiap 5 detik + auto-stop.
 *
 * Task ini SATU-SATUNYA yang menyelesaikan scan (cancel disc,
 * resume NUS adv, print STOPPED) untuk SEMUA jalur berhenti:
 *   - manual stop  → ble_scanner_stop() set s_stop_requested
 *   - durasi habis → break dari loop
 *   - DISC_COMPLETE → scan_event_cb set s_running=false
 * Dengan begitu cleanup selalu jalan tepat sekali.
 * ============================================================ */
static void progress_task(void *arg)
{
    (void)arg;
    uint32_t last_report = 0;

    while (s_running && !s_stop_requested) {
        uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us)
                                    / 1000000);

        if (sec - last_report >= 5) {
            last_report = sec;
            uint32_t count = 0;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                count = s_device_count;
                xSemaphoreGive(s_lock);
            }
            io_log("blescan: PROGRESS %us found=%" PRIu32 "\r\n",
                   (unsigned)sec, count);
        }

        /* Cek durasi habis */
        if (s_duration_sec > 0 && sec >= s_duration_sec) break;

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* ---- Finalisasi (idempotent-safe) ---- */
    s_running = false;

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "disc_cancel rc=%d", rc);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    transport_ble_resume_adv();

    uint32_t took = (uint32_t)((esp_timer_get_time() - s_started_us)
                                / 1000000);
    io_log("blescan: STOPPED total=%" PRIu32 " took=%us\r\n",
           s_device_count, (unsigned)took);

    s_stop_requested = false;
    s_progress_task = NULL;
    vTaskDelete(NULL);
}

/* ============================================================
 * Public API
 * ============================================================ */
esp_err_t ble_scanner_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_device_count = 0;
    s_running = false;
    s_stop_requested = false;
    return ESP_OK;
}

esp_err_t ble_scanner_start(uint32_t duration_sec, bool active_scan)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    if (s_running) {
        io_log("blescan: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Guard: hentikan BLE spam / spectrum kalau jalan (radio BLE) */
    if (ble_spam_is_running()) {
        io_log("blescan: stopping BLE spam first\r\n");
        ble_spam_stop();
    }
    if (ble_spectrum_is_running()) {
        io_log("blescan: stopping BLE spectrum first\r\n");
        ble_spectrum_stop();
    }
    if (wifi_spam_is_running()) {
        io_log("blescan: stopping WiFi spam first\r\n");
        wifi_spam_stop();
    }

    if (!ensure_addr()) {
        io_log("blescan: BLE not synced yet\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Reset buffer */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_device_count = 0;
        xSemaphoreGive(s_lock);
    }
    s_stop_requested = false;
    s_duration_sec = duration_sec;
    s_started_us = esp_timer_get_time();

    /* Pause NUS adv supaya RF fokus scan */
    transport_ble_pause_adv();
    vTaskDelay(pdMS_TO_TICKS(50));  /* beri waktu NimBLE settle */

    /* Scan params */
    struct ble_gap_disc_params params = {0};
    params.passive          = active_scan ? 0 : 1;
    params.filter_duplicates = 0;   /* kita mau update RSSI tiap deteksi */
    params.itvl             = 0x0010;  /* 10 ms */
    params.window           = 0x0010;  /* 10 ms window */
    params.filter_policy    = 0;
    params.limited          = 0;

    int rc = ble_gap_disc(s_own_addr_type,
                           BLE_HS_FOREVER,   /* durasi diatur manual */
                           &params,
                           scan_event_cb, NULL);
    if (rc != 0) {
        io_log("blescan: ble_gap_disc rc=%d\r\n", rc);
        transport_ble_resume_adv();
        return ESP_FAIL;
    }

    s_running = true;

    io_log("blescan: START dur=%us active=%u\r\n",
           (unsigned)duration_sec, (unsigned)active_scan);

    /* Task untuk progress report + auto-stop. Kalau gagal dibuat, tidak ada
     * yang akan meng-cancel disc / resume adv — jadi finalisasi di sini. */
    if (xTaskCreate(progress_task, "ble_scan_prog", 3072, NULL, 3,
                    &s_progress_task) != pdPASS) {
        ESP_LOGW(TAG, "progress task create failed");
        s_running = false;
        ble_gap_disc_cancel();
        transport_ble_resume_adv();
        io_log("blescan: STOPPED total=0 took=0s\r\n");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ble_scanner_stop(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!s_running && s_progress_task == NULL) {
        io_log("blescan: not running\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Request stop; progress_task performs the actual cleanup + STOPPED log
     * so finalization happens exactly once regardless of who triggered it. */
    s_stop_requested = true;

    for (int i = 0; i < 100 && s_progress_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Safety net: if the progress task never ran, finalize here. */
    if (s_progress_task == NULL && s_running) {
        s_running = false;
        ble_gap_disc_cancel();
        transport_ble_resume_adv();
    }
    return ESP_OK;
}

bool ble_scanner_is_running(void)
{
    return s_running;
}

void ble_scanner_dump(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;

    if (s_device_count == 0) {
        io_log("blescan: LS (empty)\r\n");
        xSemaphoreGive(s_lock);
        return;
    }

    io_log("blescan: LS count=%" PRIu32 "\r\n", s_device_count);
    for (uint32_t i = 0; i < s_device_count; i++) {
        const ble_scan_entry_t *e = &s_devices[i];
        char mfg_str[8];
        if (e->mfg_id == 0xFFFF) {
            snprintf(mfg_str, sizeof(mfg_str), "----");
        } else {
            snprintf(mfg_str, sizeof(mfg_str), "%04X", e->mfg_id);
        }
        io_log("blescan: LS idx=%" PRIu32 " "
               "addr=%02X:%02X:%02X:%02X:%02X:%02X "
               "rssi=%d type=%u atype=%u mfg=%s seen=%u name=\"%s\"\r\n",
               i,
               e->addr[0], e->addr[1], e->addr[2],
               e->addr[3], e->addr[4], e->addr[5],
               (int)e->rssi, e->adv_type, e->addr_type, mfg_str,
               e->seen, e->name);
    }
    io_log("blescan: LS end\r\n");

    xSemaphoreGive(s_lock);
}

void ble_scanner_clear(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_device_count = 0;
    xSemaphoreGive(s_lock);
    io_log("blescan: cleared\r\n");
}

uint32_t ble_scanner_count(void)
{
    uint32_t n = 0;
    if (!s_lock) return 0;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return 0;
    n = s_device_count;
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t ble_scanner_elapsed_sec(void)
{
    if (s_started_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
}
