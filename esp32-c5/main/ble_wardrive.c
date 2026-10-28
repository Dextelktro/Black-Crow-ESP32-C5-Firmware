/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ble_wardrive.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

#include "io.h"
#include "transport_ble.h"
#include "ble_scanner.h"
#include "ble_spam.h"
#include "ble_spectrum.h"
#include "wifi_spam.h"
#include "storage_fs.h"

/* ─────────────────────────────────────────────────────────────────────────
 * BLE wardriving engine.
 *
 * NimBLE discovery already streams every advertising report through a GAP
 * callback when filter_duplicates=0, so — unlike WiFi — no channel hopping is
 * needed; the controller hops the three BLE advertising channels for us. The
 * callback parses + classifies each report into an in-memory table; a reporter
 * task flushes that table every FLUSH_MS, emitting NEW for first sightings,
 * UPD for significant RSSI moves and GONE for devices silent past the aging
 * timeout. A device therefore appears in the app within ~one flush of its
 * first advertisement. The reporter task is also the single owner of scan
 * finalization (cancel disc, resume NUS adv, print STOPPED) for every stop
 * path — manual stop, duration expiry, or DISC_COMPLETE.
 * ──────────────────────────────────────────────────────────────────────── */

#define BLEWAR_MAX_DEVICES   128
#define BLEWAR_NAME_LEN      32
#define RSSI_DELTA_THRESH    6
#define GONE_TIMEOUT_US      12000000LL   /* 12 s silent → GONE */
#define FLUSH_MS             400          /* NEW/UPD/GONE cadence */
#define CYCLE_MS             2000         /* CYCLE summary cadence */
#define NAME_GRACE_US        700000LL     /* wait this long for a name before
                                           * emitting NEW (BLE names often arrive
                                           * in a later SCAN_RSP) */

typedef struct {
    bool     used;
    bool     reported;              /* NEW already emitted */
    bool     meta_dirty;            /* name/vendor/dtype refined after NEW */
    bool     airtag_format;         /* Apple Find My lost-mode payload shape */
    uint8_t  addr[6];
    uint8_t  addr_type;             /* 0=public, 1=random */
    int8_t   rssi;
    int8_t   last_rep_rssi;
    uint16_t mfg_id;                /* 0xFFFF = none */
    char     name[BLEWAR_NAME_LEN];
    char     vendor[12];
    char     dtype[12];
    char     priv[8];
    uint32_t seen;
    int64_t  first_seen_us;
    int64_t  last_seen_us;
} blewar_entry_t;

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static volatile bool     s_stop_requested = false;

static uint8_t   s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool      s_addr_ready = false;
static bool      s_active = false;
static uint32_t  s_duration_sec = 0;
static int64_t   s_started_us = 0;
static uint32_t  s_cycle_count = 0;
static uint32_t  s_total_unique = 0;

/* Keep the latest scan table available to save_findmy after the scan stops. */
static blewar_entry_t *s_tbl;

static const char *TAG = "ble_wardrive";

/* ── classification ──────────────────────────────────────────────────────── */

/* Bluetooth SIG assigned company identifiers (manufacturer-data first 2 bytes,
 * little-endian). Only well-established IDs are mapped; anything else is
 * reported by its raw hex so no guessed vendor is ever shown. */
static const char *vendor_from_company(uint16_t id)
{
    switch (id) {
        case 0x004C: return "apple";
        case 0x00E0: return "google";
        case 0x0006: return "microsoft";
        case 0x0075: return "samsung";
        case 0x0059: return "nordic";
        case 0x02E5: return "espressif";
        default:     return NULL;
    }
}

static const char *priv_kind(uint8_t addr_type, const uint8_t *addr)
{
    if (addr_type == 0) return "pub";
    /* NimBLE stores the address little-endian, so addr[5] is the MSB; the top
     * two bits classify a random address. */
    switch (addr[5] >> 6) {
        case 0x3: return "static";
        case 0x1: return "rpa";
        case 0x0: return "nrpa";
        default:  return "rnd";
    }
}

/* Parse one advertising payload → name, company id, vendor and device type.
 * vendor/dtype default to "-" when unknown. */
static void classify(const uint8_t *data, uint8_t len,
                     char *name, size_t name_max,
                     uint16_t *mfg_out,
                     char *vendor, size_t vendor_max,
                     char *dtype, size_t dtype_max,
                     bool *airtag_format_out)
{
    name[0] = '\0';
    vendor[0] = '-'; vendor[1] = '\0';
    dtype[0]  = '-'; dtype[1]  = '\0';

    uint16_t company = 0xFFFF;
    uint8_t  apple_type = 0xFF;
    bool     airtag_format = false;
    uint16_t svc_hit = 0;         /* notable 16-bit service UUID */

    size_t i = 0;
    while (i + 1 < len) {
        uint8_t flen = data[i];
        if (flen == 0) break;
        if (i + 1 + flen > len) break;
        uint8_t ftype = data[i + 1];
        const uint8_t *val = data + i + 2;
        uint8_t vlen = flen - 1;

        switch (ftype) {
            case 0x08:            /* Shortened Local Name */
            case 0x09:            /* Complete Local Name */
                if (vlen > 0) {
                    size_t n = vlen < name_max - 1 ? vlen : name_max - 1;
                    for (size_t k = 0; k < n; k++) {
                        uint8_t c = val[k];
                        /* keep the line protocol intact */
                        name[k] = (c < 0x20 || c == '"') ? ' ' : (char)c;
                    }
                    name[n] = '\0';
                }
                break;
            case 0xFF:            /* Manufacturer Specific Data */
                if (vlen >= 2) {
                    company = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
                    apple_type = 0xFF;
                    airtag_format = false;
                    if (company == 0x004C && vlen >= 3) {
                        apple_type = val[2];
                        /* Apple subtype 0x12 is shared by many Find My
                         * devices. Only flag the known 29-byte lost-mode
                         * record shape for the dedicated AirTag screen. */
                        airtag_format = apple_type == 0x12 &&
                                        vlen >= 29 && val[3] == 0x19;
                    }
                }
                break;
            case 0x02:            /* Incomplete 16-bit Service UUIDs */
            case 0x03:            /* Complete 16-bit Service UUIDs */
            case 0x14:            /* 16-bit Service Solicitation */
                for (uint8_t k = 0; k + 1 < vlen; k += 2) {
                    uint16_t u = (uint16_t)val[k] | ((uint16_t)val[k + 1] << 8);
                    if (u == 0xFE2C || u == 0xFD6F || u == 0xFEED || u == 0xFEAA)
                        svc_hit = u;
                }
                break;
            case 0x16:            /* Service Data — 16-bit UUID */
                if (vlen >= 2) {
                    uint16_t u = (uint16_t)val[0] | ((uint16_t)val[1] << 8);
                    if (u == 0xFE2C || u == 0xFD6F || u == 0xFEED || u == 0xFEAA)
                        svc_hit = u;
                }
                break;
            default:
                break;
        }
        i += 1 + flen;
    }

    *mfg_out = company;
    *airtag_format_out = airtag_format;

    const char *vn = vendor_from_company(company);
    if (vn) { strncpy(vendor, vn, vendor_max - 1); vendor[vendor_max - 1] = '\0'; }

    /* Device type: service-UUID fingerprints first, then Apple Continuity. */
    const char *dt = NULL;
    switch (svc_hit) {
        case 0xFE2C: dt = "fastpair";  break;
        case 0xFD6F: dt = "exposure";  break;
        case 0xFEED: dt = "tile";
            if (!vn) { strncpy(vendor, "tile", vendor_max - 1);
                       vendor[vendor_max - 1] = '\0'; }
            break;
        case 0xFEAA: dt = "eddystone"; break;
        default: break;
    }
    if (!dt && company == 0x004C) {
        switch (apple_type) {
            case 0x05: dt = "airdrop"; break;
            case 0x07: dt = "airpods"; break;
            case 0x0B: dt = "watch";   break;
            case 0x0C: dt = "handoff"; break;
            case 0x10: dt = "nearby";  break;
            case 0x12: dt = "findmy";  break;
            default:   dt = "apple";   break;
        }
    }
    if (dt) { strncpy(dtype, dt, dtype_max - 1); dtype[dtype_max - 1] = '\0'; }
}

/* ── table ───────────────────────────────────────────────────────────────── */

static bool ensure_addr(void)
{
    if (s_addr_ready) return true;
    if (ble_hs_util_ensure_addr(0) != 0) return false;
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) return false;
    s_addr_ready = true;
    return true;
}

/* Called from the NimBLE host task for every advertising report. Updates the
 * table only; all io_log happens later in the reporter task. */
static void handle_report(const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                          const uint8_t *data, uint8_t len)
{
    char     name[BLEWAR_NAME_LEN];
    char     vendor[12];
    char     dtype[12];
    uint16_t mfg_id;
    bool     airtag_format;
    classify(data, len, name, sizeof(name), &mfg_id,
             vendor, sizeof(vendor), dtype, sizeof(dtype), &airtag_format);

    int64_t now = esp_timer_get_time();

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;

    int found = -1, free_slot = -1;
    for (int i = 0; i < BLEWAR_MAX_DEVICES; i++) {
        if (!s_tbl[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (s_tbl[i].addr_type == addr_type &&
            memcmp(s_tbl[i].addr, addr, 6) == 0) { found = i; break; }
    }

    if (found >= 0) {
        blewar_entry_t *e = &s_tbl[found];
        e->rssi = rssi;
        e->last_seen_us = now;
        if (e->seen < UINT32_MAX) e->seen++;
        if (e->name[0] == '\0' && name[0] != '\0') {
            memcpy(e->name, name, sizeof(e->name));
            e->meta_dirty = true;
        }
        if (e->mfg_id == 0xFFFF && mfg_id != 0xFFFF) {
            e->mfg_id = mfg_id;
            e->meta_dirty = true;
        }
        if (e->vendor[0] == '-' && vendor[0] != '-') {
            memcpy(e->vendor, vendor, sizeof(e->vendor));
            e->meta_dirty = true;
        }
        if (e->dtype[0] == '-' && dtype[0] != '-') {
            memcpy(e->dtype, dtype, sizeof(e->dtype));
            e->meta_dirty = true;
        } else if (e->mfg_id == 0x004C && mfg_id == 0x004C &&
                   strcmp(e->dtype, "apple") == 0 &&
                   strcmp(dtype, "findmy") == 0) {
            memcpy(e->dtype, dtype, sizeof(e->dtype));
            e->meta_dirty = true;
        }
        e->airtag_format = e->airtag_format || airtag_format;
    } else if (free_slot >= 0) {
        blewar_entry_t *e = &s_tbl[free_slot];
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->reported = false;
        memcpy(e->addr, addr, 6);
        e->addr_type = addr_type;
        e->rssi = rssi;
        e->last_rep_rssi = rssi;
        e->mfg_id = mfg_id;
        e->airtag_format = airtag_format;
        memcpy(e->name, name, sizeof(e->name));
        memcpy(e->vendor, vendor, sizeof(e->vendor));
        memcpy(e->dtype, dtype, sizeof(e->dtype));
        const char *pk = priv_kind(addr_type, addr);
        strncpy(e->priv, pk, sizeof(e->priv) - 1);
        e->seen = 1;
        e->first_seen_us = now;
        e->last_seen_us = now;
    }

    xSemaphoreGive(s_lock);
}

/* ── GAP event callback ──────────────────────────────────────────────────── */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
#if MYNEWT_VAL(BLE_EXT_ADV)
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
        if (d->length_data > 0)
            handle_report(d->addr.val, d->addr.type, d->rssi,
                          d->data, d->length_data);
        break;
    }
#else
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;
        if (d->length_data > 0)
            handle_report(d->addr.val, d->addr.type, d->rssi,
                          d->data, d->length_data);
        break;
    }
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_running = false;
        break;
    default:
        break;
    }
    return 0;
}

/* ── reporting ───────────────────────────────────────────────────────────── */

static void flush(int64_t now, int *p_new, int *p_upd, int *p_gone)
{
    for (int i = 0; i < BLEWAR_MAX_DEVICES; i++) {
        uint8_t  addr[6];
        char     name[BLEWAR_NAME_LEN];
        char     vendor[12], dtype[12], priv[8];
        uint8_t  atype = 0;
        uint16_t mfg_id = 0xFFFF;
        int8_t   rssi = 0, old_rssi = 0;
        int      action = 0;     /* 1=NEW 2=UPD 3=GONE */
        bool     emit_meta = false;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (!s_tbl[i].used) { xSemaphoreGive(s_lock); continue; }

        memcpy(addr, s_tbl[i].addr, 6);
        memcpy(name, s_tbl[i].name, sizeof(name));
        memcpy(vendor, s_tbl[i].vendor, sizeof(vendor));
        memcpy(dtype, s_tbl[i].dtype, sizeof(dtype));
        memcpy(priv, s_tbl[i].priv, sizeof(priv));
        atype    = s_tbl[i].addr_type;
        mfg_id   = s_tbl[i].mfg_id;
        rssi     = s_tbl[i].rssi;
        old_rssi = s_tbl[i].last_rep_rssi;

        if (now - s_tbl[i].last_seen_us > GONE_TIMEOUT_US) {
            s_tbl[i].used = false;
            action = 3;
        } else if (!s_tbl[i].reported) {
            /* Hold the first report briefly so a name arriving in a later
             * SCAN_RSP is included — but never stall a no-name device past
             * the grace window. */
            if (s_tbl[i].name[0] != '\0' ||
                now - s_tbl[i].first_seen_us >= NAME_GRACE_US) {
                s_tbl[i].reported = true;
                s_tbl[i].last_rep_rssi = rssi;
                action = 1;
            }
            /* else: defer — entry stays unreported, retried next flush. */
        } else {
            int d = (int)rssi - (int)old_rssi;
            if (d < 0) d = -d;
            if (d >= RSSI_DELTA_THRESH) {
                s_tbl[i].last_rep_rssi = rssi;
                action = 2;
            }
        }

        /* Push a late-arriving name/vendor/dtype refinement to the app. NEW
         * already carries the current metadata, so only fire META when the
         * device was reported earlier and is not being removed. */
        if (action == 1) {
            s_tbl[i].meta_dirty = false;
        } else if (action != 3 && s_tbl[i].reported && s_tbl[i].meta_dirty) {
            s_tbl[i].meta_dirty = false;
            emit_meta = true;
        }
        xSemaphoreGive(s_lock);

        if (action == 1) {
            char comp[8];
            if (mfg_id == 0xFFFF) snprintf(comp, sizeof(comp), "----");
            else                  snprintf(comp, sizeof(comp), "%04X", mfg_id);
            io_log("blewar: NEW addr=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d "
                   "atype=%u priv=%s vendor=%s dtype=%s company=%s name=\"%s\"\r\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                   (int)rssi, atype, priv, vendor, dtype, comp, name);
            s_total_unique++;
            if (p_new) (*p_new)++;
        } else if (action == 2) {
            io_log("blewar: UPD addr=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d->%d\r\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                   (int)old_rssi, (int)rssi);
            if (p_upd) (*p_upd)++;
        } else if (action == 3) {
            io_log("blewar: GONE addr=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            if (p_gone) (*p_gone)++;
        }

        if (emit_meta) {
            char comp[8];
            if (mfg_id == 0xFFFF) snprintf(comp, sizeof(comp), "----");
            else                  snprintf(comp, sizeof(comp), "%04X", mfg_id);
            io_log("blewar: META addr=%02X:%02X:%02X:%02X:%02X:%02X "
                   "vendor=%s dtype=%s company=%s name=\"%s\"\r\n",
                   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                   vendor, dtype, comp, name);
        }
    }
}

static void emit_findmy_observations(void)
{
    for (int i = 0; i < BLEWAR_MAX_DEVICES; i++) {
        blewar_entry_t entry;
        bool eligible = false;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        if (s_tbl[i].used && s_tbl[i].reported &&
            s_tbl[i].airtag_format && s_tbl[i].mfg_id == 0x004C &&
            strcmp(s_tbl[i].dtype, "findmy") == 0) {
            entry = s_tbl[i];
            eligible = true;
        }
        xSemaphoreGive(s_lock);
        if (!eligible) continue;

        io_log("blefind: OBS addr=%02X:%02X:%02X:%02X:%02X:%02X "
               "seen=%u rssi=%d format=airtag-v1\r\n",
               entry.addr[0], entry.addr[1], entry.addr[2],
               entry.addr[3], entry.addr[4], entry.addr[5],
               (unsigned)entry.seen, (int)entry.rssi);
    }
}

static int used_count(void)
{
    int c = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    for (int i = 0; i < BLEWAR_MAX_DEVICES; i++) if (s_tbl[i].used) c++;
    xSemaphoreGive(s_lock);
    return c;
}

static void reporter_task(void *arg)
{
    (void)arg;
    int n_new = 0, n_upd = 0, n_gone = 0;
    uint32_t since_cycle_ms = 0;

    while (s_running && !s_stop_requested) {
        flush(esp_timer_get_time(), &n_new, &n_upd, &n_gone);

        since_cycle_ms += FLUSH_MS;
        if (since_cycle_ms >= CYCLE_MS) {
            since_cycle_ms = 0;
            s_cycle_count++;
            emit_findmy_observations();
            io_log("blewar: CYCLE %" PRIu32 " total=%d new=%d upd=%d gone=%d\r\n",
                   s_cycle_count, used_count(), n_new, n_upd, n_gone);
            n_new = n_upd = n_gone = 0;
        }

        if (s_duration_sec > 0) {
            uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us)
                                        / 1000000);
            if (sec >= s_duration_sec) {
                io_log("blewar: duration reached\r\n");
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));
    }

    /* ---- Finalize exactly once (manual stop / duration / DISC_COMPLETE). --- */
    s_running = false;
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGW(TAG, "disc_cancel rc=%d", rc);
    vTaskDelay(pdMS_TO_TICKS(20));
    transport_ble_resume_adv();

    io_log("blewar: STOPPED cycles=%" PRIu32 " unique=%" PRIu32 "\r\n",
           s_cycle_count, s_total_unique);

    s_stop_requested = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ── public API ──────────────────────────────────────────────────────────── */

esp_err_t ble_wardrive_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    if (!s_tbl) {
        s_tbl = heap_caps_calloc(BLEWAR_MAX_DEVICES, sizeof(*s_tbl),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_tbl) {
            ESP_LOGE(TAG, "BLE wardrive result PSRAM allocation failed");
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_running = false;
    s_stop_requested = false;
    return ESP_OK;
}

esp_err_t ble_wardrive_start(bool active, uint32_t duration_sec)
{
    if (!s_lock || !s_tbl) return ESP_ERR_INVALID_STATE;
    if (s_running || s_task != NULL) {
        io_log("blewar: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* Yield the other BLE radio users — only one discovery can run. */
    if (ble_scanner_is_running())  ble_scanner_stop();
    if (ble_spam_is_running())     ble_spam_stop();
    if (ble_spectrum_is_running()) ble_spectrum_stop();
    if (wifi_spam_is_running())    wifi_spam_stop();

    if (!ensure_addr()) {
        io_log("blewar: BLE not synced yet\r\n");
        /* Terminal line so the app clears its optimistic running state. */
        io_log("blewar: STOPPED cycles=0 unique=0\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(s_tbl, 0, BLEWAR_MAX_DEVICES * sizeof(*s_tbl));
        xSemaphoreGive(s_lock);
    }
    s_active         = active;
    s_duration_sec   = duration_sec;
    s_stop_requested = false;
    s_cycle_count    = 0;
    s_total_unique   = 0;
    s_started_us     = esp_timer_get_time();

    transport_ble_pause_adv();
    vTaskDelay(pdMS_TO_TICKS(50));

    struct ble_gap_disc_params params = {0};
    params.passive           = active ? 0 : 1;
    params.filter_duplicates = 0;     /* we want continuous RSSI updates */
    params.itvl              = 0x0010;
    params.window            = 0x0010;
    params.filter_policy     = 0;
    params.limited           = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                          gap_event_cb, NULL);
    if (rc != 0) {
        io_log("blewar: ble_gap_disc rc=%d\r\n", rc);
        transport_ble_resume_adv();
        io_log("blewar: STOPPED cycles=0 unique=0\r\n");
        return ESP_FAIL;
    }

    s_running = true;
    io_log("blewar: START active=%u dur=%us\r\n",
           (unsigned)active, (unsigned)duration_sec);

    if (xTaskCreate(reporter_task, "blewar", 4096, NULL, 3, &s_task) != pdPASS) {
        s_running = false;
        ble_gap_disc_cancel();
        transport_ble_resume_adv();
        io_log("blewar: STOPPED cycles=0 unique=0\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ble_wardrive_stop(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!s_running && s_task == NULL) {
        io_log("blewar: not running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Safety net if the reporter task never ran. */
    if (s_task == NULL && s_running) {
        s_running = false;
        ble_gap_disc_cancel();
        transport_ble_resume_adv();
    }
    return ESP_OK;
}

bool     ble_wardrive_is_running(void)   { return s_running; }
uint32_t ble_wardrive_total_unique(void) { return s_total_unique; }
uint32_t ble_wardrive_cycle_count(void)  { return s_cycle_count; }

esp_err_t ble_wardrive_save_findmy(const char *address)
{
    if (!s_lock || !s_tbl) return ESP_ERR_INVALID_STATE;
    if (!address || strlen(address) != 17)
        return ESP_ERR_INVALID_ARG;

    blewar_entry_t entry;
    bool found = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    for (int i = 0; i < BLEWAR_MAX_DEVICES; i++) {
        if (!s_tbl[i].used || !s_tbl[i].reported ||
            !s_tbl[i].airtag_format || s_tbl[i].mfg_id != 0x004C ||
            strcmp(s_tbl[i].dtype, "findmy") != 0) {
            continue;
        }
        char candidate[18];
        snprintf(candidate, sizeof(candidate), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_tbl[i].addr[0], s_tbl[i].addr[1], s_tbl[i].addr[2],
                 s_tbl[i].addr[3], s_tbl[i].addr[4], s_tbl[i].addr[5]);
        if (strcasecmp(candidate, address) == 0) {
            entry = s_tbl[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!found) return ESP_ERR_NOT_FOUND;

    esp_err_t err = storage_fs_ready() ? ESP_OK : storage_fs_mount();
    if (err != ESP_OK) return err;

    const char *path = STORAGE_FS_BASE "/findmy_observations.csv";
    struct stat st;
    bool needs_header = stat(path, &st) != 0 || st.st_size == 0;
    FILE *file = fopen(path, "a");
    if (!file) return ESP_FAIL;

    if (needs_header &&
        fputs("uptime_ms,address,company,type,rssi_dbm,advertisements,name\n",
              file) == EOF) {
        fclose(file);
        return ESP_FAIL;
    }

    char candidate[18];
    snprintf(candidate, sizeof(candidate), "%02X:%02X:%02X:%02X:%02X:%02X",
             entry.addr[0], entry.addr[1], entry.addr[2],
             entry.addr[3], entry.addr[4], entry.addr[5]);
    uint64_t uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    int written = fprintf(file, "%" PRIu64 ",%s,004C,airtag-format,%d,%u,\"%s\"\n",
                          uptime_ms, candidate, (int)entry.rssi,
                          (unsigned)entry.seen, entry.name);
    int close_rc = fclose(file);
    return written < 0 || close_rc != 0 ? ESP_FAIL : ESP_OK;
}
