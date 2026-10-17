#include "wifi_live_scan.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "io.h"
#include "targets.h"        /* TARGETS_MAX_APS */

/* ─────────────────────────────────────────────────────────────────────────
 * Wardriving-style live scan.
 *
 * Instead of the old batch model (full dual-band esp_wifi_scan_start sweep,
 * emit results only when the whole sweep finished, then sleep a fixed
 * interval), this is a continuous promiscuous beacon capture:
 *
 *   - Promiscuous mode with an MGMT-only filter + an RX callback.
 *   - A hop task walks the channel plan with a short per-channel dwell and
 *     NEVER idles between sweeps.
 *   - Every captured beacon / probe-response updates an in-memory table.
 *   - After each channel dwell the table is flushed: APs heard for the first
 *     time emit NEW, significant RSSI moves emit UPD, and APs not heard for
 *     GONE_TIMEOUT emit GONE. A new AP therefore shows up in the app within
 *     ~one dwell (~120 ms) of its first beacon — real time, no delay.
 *
 * The wire protocol (NEW / UPD / GONE / CYCLE / START / STOPPED) is unchanged,
 * so cli.c and the Flutter parser need no edits. interval_ms is accepted for
 * API compatibility but ignored — capture is continuous by design.
 * ──────────────────────────────────────────────────────────────────────── */

#define WLSCAN_MAX_APS      TARGETS_MAX_APS
#define RSSI_DELTA_THRESH   6
#define GONE_TIMEOUT_US     12000000LL   /* 12 s silent → GONE */

#define MGMT_SUBTYPE_PROBE_REQ   4
#define MGMT_SUBTYPE_PROBE_RESP  5
#define MGMT_SUBTYPE_BEACON      8
#define MGMT_SUBTYPE_DISASSOC    10
#define MGMT_SUBTYPE_AUTH        11
#define MGMT_SUBTYPE_DEAUTH      12

/* Passive air monitor (Marauder-style WiFi sniffer): classify every captured
 * management/data frame so the app can draw a real-time frame-rate monitor
 * under the Nuke radar and flag a deauth/disassoc flood — i.e. an over-the-air
 * attack happening around the device. A telemetry line is emitted ~1 Hz. */
#define AIR_EMIT_INTERVAL_US   1000000LL
#define AIR_ALERT_THRESH       3     /* deauth+disassoc in one window → alert */

typedef struct {
    bool     used;
    bool     reported;        /* NEW already emitted */
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    int8_t   last_rep_rssi;   /* RSSI when we last emitted NEW/UPD */
    bool     band_5ghz;
    int64_t  last_seen_us;
} wl_entry_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wl_entry_t   s_tbl[WLSCAN_MAX_APS];
static target_ap_t *s_target_snapshot;

static TaskHandle_t  s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_stop_requested = false;

static wlscan_mode_t    s_mode = WLSCAN_MODE_FAST;
static uint32_t         s_duration_sec = 0;
static uint32_t         s_cycle_count = 0;
static uint32_t         s_total_unique = 0;

/* Radio tuning state (hop task only, except s_cur_channel read by callback). */
static int               s_cur_band = 0;    /* 0=none, 24, 5 */
static bool              s_5g_ok = true;
static volatile uint8_t  s_cur_channel = 1;

/* Non-overlapping 2.4 GHz first, then the rest; common 5 GHz UNII channels. */
static const uint8_t CH_24[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
static const uint8_t CH_5[]  = {36, 40, 44, 48, 149, 153, 157, 161};

/* Air-monitor counters. Written ONLY by the Wi-Fi RX callback (single writer),
 * so the hop task can snapshot them lock-free for the ~1 Hz AIR telemetry.
 * They are monotonic for the lifetime of a scan and reset when it (re)starts. */
typedef struct {
    volatile uint32_t total;
    volatile uint32_t mgmt;
    volatile uint32_t data;
    volatile uint32_t beacon;
    volatile uint32_t probe;
    volatile uint32_t deauth;
    volatile uint32_t disassoc;
    volatile uint32_t auth;
    volatile uint32_t eapol;
} air_counters_t;

static air_counters_t s_air;
static air_counters_t s_air_snap;       /* counter values at the last AIR emit */
static int64_t        s_air_last_emit_us;

/* ── capture callback (runs in the Wi-Fi task) ───────────────────────────── */

/* Tally one captured frame for the air monitor. Management frames are split by
 * subtype (beacon / probe / deauth / disassoc / auth); data frames are counted
 * and sniffed for the LLC/SNAP EAPOL ethertype (handshake / PMKID traffic). */
static void air_count(uint8_t ftype, uint8_t subtype,
                      const uint8_t *f, uint16_t len)
{
    s_air.total++;
    if (ftype == 0) {                           /* management */
        s_air.mgmt++;
        switch (subtype) {
            case MGMT_SUBTYPE_BEACON:     s_air.beacon++;   break;
            case MGMT_SUBTYPE_PROBE_REQ:
            case MGMT_SUBTYPE_PROBE_RESP: s_air.probe++;    break;
            case MGMT_SUBTYPE_DEAUTH:     s_air.deauth++;   break;
            case MGMT_SUBTYPE_DISASSOC:   s_air.disassoc++; break;
            case MGMT_SUBTYPE_AUTH:       s_air.auth++;     break;
            default: break;
        }
    } else if (ftype == 2) {                    /* data */
        s_air.data++;
        /* EAPOL lives behind the MAC header + optional QoS field + LLC/SNAP. */
        uint8_t fc1 = f[1];
        bool toDS = fc1 & 0x01, fromDS = fc1 & 0x02;
        int hdr = 24;
        if (toDS && fromDS) hdr += 6;           /* 4-address (WDS) frame */
        if (subtype & 0x08) hdr += 2;           /* QoS data carries a QoS ctl */
        if (hdr + 8 <= (int)len) {
            const uint8_t *s = f + hdr;
            if (s[0] == 0xAA && s[1] == 0xAA && s[2] == 0x03 &&
                s[6] == 0x88 && s[7] == 0x8E) { /* SNAP + ethertype 0x888E */
                s_air.eapol++;
            }
        }
    }
}

static void wl_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    uint16_t len = p->rx_ctrl.sig_len;
    if (len < 10) return;                       /* need frame control + addrs */
    const uint8_t *f = p->payload;

    uint8_t fc0     = f[0];
    uint8_t ftype   = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0xF;

    /* Air monitor first — it classifies both management and data frames. */
    air_count(ftype, subtype, f, len);

    /* AP table is built from beacons / probe responses only. */
    if (type != WIFI_PKT_MGMT) return;
    if (ftype != 0) return;
    if (subtype != MGMT_SUBTYPE_BEACON && subtype != MGMT_SUBTYPE_PROBE_RESP)
        return;
    if (len < 36) return;                       /* 24 hdr + 12 fixed params */

    const uint8_t *bssid = f + 16;              /* addr3 = BSSID */

    /* Walk tagged IEs for SSID (tag 0) and DS Parameter Set (tag 3). */
    char    ssid[33];
    ssid[0] = '\0';
    uint8_t ds_ch = 0;
    int pos = 36;
    while (pos + 2 <= (int)len) {
        uint8_t tag  = f[pos];
        uint8_t tlen = f[pos + 1];
        if (pos + 2 + (int)tlen > (int)len) break;
        const uint8_t *val = f + pos + 2;
        if (tag == 0) {                         /* SSID */
            int sl = tlen;
            if (sl > 32) sl = 32;
            memcpy(ssid, val, sl);
            ssid[sl] = '\0';
            bool printable = false;
            for (int k = 0; k < sl; k++) {
                if (ssid[k] != '\0' && ssid[k] != ' ') { printable = true; break; }
            }
            if (!printable) ssid[0] = '\0';     /* hidden */
        } else if (tag == 3 && tlen >= 1) {     /* DS Parameter Set */
            ds_ch = val[0];
        }
        pos += 2 + (int)tlen;
    }

    uint8_t ch    = (ds_ch >= 1 && ds_ch <= 14) ? ds_ch : s_cur_channel;
    bool    band5 = (ch > 14);
    int8_t  rssi  = p->rx_ctrl.rssi;
    int64_t now   = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    int found = -1, free_slot = -1;
    for (int i = 0; i < WLSCAN_MAX_APS; i++) {
        if (!s_tbl[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (memcmp(s_tbl[i].bssid, bssid, 6) == 0) { found = i; break; }
    }
    if (found >= 0) {
        wl_entry_t *e = &s_tbl[found];
        e->rssi = rssi;
        e->channel = ch;
        e->band_5ghz = band5;
        e->last_seen_us = now;
        if (e->ssid[0] == '\0' && ssid[0] != '\0')
            memcpy(e->ssid, ssid, sizeof(e->ssid));
    } else if (free_slot >= 0) {
        wl_entry_t *e = &s_tbl[free_slot];
        e->used = true;
        e->reported = false;
        memcpy(e->bssid, bssid, 6);
        memcpy(e->ssid, ssid, sizeof(e->ssid));
        e->channel = ch;
        e->rssi = rssi;
        e->last_rep_rssi = rssi;
        e->band_5ghz = band5;
        e->last_seen_us = now;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ── reporting ───────────────────────────────────────────────────────────── */

/* Emit NEW / UPD / GONE for the current table state. Decisions are made under
 * the lock; io_log always runs with the lock released. */
static void wl_flush(int64_t now, int *p_new, int *p_upd, int *p_gone)
{
    for (int i = 0; i < WLSCAN_MAX_APS; i++) {
        uint8_t bssid[6];
        char    ssid[33];
        uint8_t ch = 0;
        int8_t  rssi = 0, old_rssi = 0;
        bool    band5 = false;
        int     action = 0;      /* 1=NEW 2=UPD 3=GONE */

        portENTER_CRITICAL(&s_mux);
        if (!s_tbl[i].used) { portEXIT_CRITICAL(&s_mux); continue; }
        memcpy(bssid, s_tbl[i].bssid, 6);
        memcpy(ssid,  s_tbl[i].ssid, sizeof(ssid));
        ch       = s_tbl[i].channel;
        rssi     = s_tbl[i].rssi;
        band5    = s_tbl[i].band_5ghz;
        old_rssi = s_tbl[i].last_rep_rssi;
        if (now - s_tbl[i].last_seen_us > GONE_TIMEOUT_US) {
            s_tbl[i].used = false;
            action = 3;
        } else if (!s_tbl[i].reported) {
            s_tbl[i].reported = true;
            s_tbl[i].last_rep_rssi = rssi;
            action = 1;
        } else {
            int d = (int)rssi - (int)old_rssi;
            if (d < 0) d = -d;
            if (d >= RSSI_DELTA_THRESH) {
                s_tbl[i].last_rep_rssi = rssi;
                action = 2;
            }
        }
        portEXIT_CRITICAL(&s_mux);

        if (action == 1) {
            io_log("wlscan: NEW ssid=%s bssid=%02X:%02X:%02X:%02X:%02X:%02X "
                   "ch=%u rssi=%d band=%s\r\n",
                   ssid[0] ? ssid : "(hidden)",
                   bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                   (unsigned)ch, (int)rssi, band5 ? "5" : "2.4");
            s_total_unique++;
            if (p_new) (*p_new)++;
        } else if (action == 2) {
            io_log("wlscan: UPD bssid=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d->%d\r\n",
                   bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                   (int)old_rssi, (int)rssi);
            if (p_upd) (*p_upd)++;
        } else if (action == 3) {
            io_log("wlscan: GONE bssid=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
            if (p_gone) (*p_gone)++;
        }
    }
}

static int wl_used_count(void)
{
    int c = 0;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < WLSCAN_MAX_APS; i++) if (s_tbl[i].used) c++;
    portEXIT_CRITICAL(&s_mux);
    return c;
}

/* Emit one AIR telemetry line with the frames seen since the last emit. Called
 * from the hop task only. Counts are per-window; the app derives per-second
 * rates from win (ms). alert flags a likely deauth/disassoc flood. */
static void air_emit(int64_t now)
{
    uint32_t win_ms = (uint32_t)((now - s_air_last_emit_us) / 1000);
    if (win_ms == 0) win_ms = 1;

    uint32_t d_tot    = s_air.total    - s_air_snap.total;
    uint32_t d_mgmt   = s_air.mgmt     - s_air_snap.mgmt;
    uint32_t d_data   = s_air.data     - s_air_snap.data;
    uint32_t d_beacon = s_air.beacon   - s_air_snap.beacon;
    uint32_t d_probe  = s_air.probe    - s_air_snap.probe;
    uint32_t d_deauth = s_air.deauth   - s_air_snap.deauth;
    uint32_t d_disas  = s_air.disassoc - s_air_snap.disassoc;
    uint32_t d_auth   = s_air.auth     - s_air_snap.auth;
    uint32_t d_eapol  = s_air.eapol    - s_air_snap.eapol;

    s_air_snap = s_air;
    s_air_last_emit_us = now;

    uint32_t dpps =
        (uint32_t)(((uint64_t)(d_deauth + d_disas) * 1000) / win_ms);
    int alert = (d_deauth + d_disas) >= AIR_ALERT_THRESH ? 1 : 0;

    io_log("wlscan: AIR win=%" PRIu32 " tot=%" PRIu32 " mgmt=%" PRIu32
           " data=%" PRIu32 " beacon=%" PRIu32 " probe=%" PRIu32
           " deauth=%" PRIu32 " disassoc=%" PRIu32 " auth=%" PRIu32
           " eapol=%" PRIu32 " dpps=%" PRIu32 " alert=%d\r\n",
           win_ms, d_tot, d_mgmt, d_data, d_beacon, d_probe,
           d_deauth, d_disas, d_auth, d_eapol, dpps, alert);
}

/* Mirror the live table into the shared target list, so a Nuke / Deauth-All
 * launched straight from the radar fires at exactly the APs on screen.
 * Only the wlscan task calls this. Copy each entry under the spinlock into a
 * small internal stack temporary, then write the PSRAM snapshot after leaving
 * the critical section. targets_set_aps() also runs after releasing s_mux. */
static void wl_sync_targets(void)
{
    uint16_t n = 0;

    for (int i = 0; i < WLSCAN_MAX_APS; i++) {
        target_ap_t entry;
        bool used;

        portENTER_CRITICAL(&s_mux);
        used = s_tbl[i].used;
        if (used) {
            memcpy(entry.bssid, s_tbl[i].bssid, 6);
            memcpy(entry.ssid,  s_tbl[i].ssid, sizeof(entry.ssid));
            entry.channel   = s_tbl[i].channel;
            entry.rssi      = s_tbl[i].rssi;
            entry.band_5ghz = s_tbl[i].band_5ghz;
        }
        portEXIT_CRITICAL(&s_mux);

        if (used) s_target_snapshot[n++] = entry;
    }

    targets_set_aps(s_target_snapshot, n);
}

/* ── radio control ───────────────────────────────────────────────────────── */

static bool wl_tune(uint8_t ch)
{
    int band = (ch > 14) ? 5 : 24;
    if (band != s_cur_band) {
        wifi_band_mode_t bm = (band == 5) ? WIFI_BAND_MODE_5G_ONLY
                                          : WIFI_BAND_MODE_2G_ONLY;
        if (esp_wifi_set_band_mode(bm) != ESP_OK) {
            if (band == 5) s_5g_ok = false;
            return false;
        }
        s_cur_band = band;
    }
    if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        if (band == 5) s_5g_ok = false;
        return false;
    }
    s_cur_channel = ch;
    return true;
}

static esp_err_t wl_promis_start(void)
{
    wifi_promiscuous_filter_t filt = {
        /* MGMT builds the AP table; DATA feeds the air monitor + EAPOL sniff. */
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filt);
    if (err != ESP_OK) {
        io_log("wlscan: set filter failed (err=%d)\r\n", (int)err);
        return err;
    }
    err = esp_wifi_set_promiscuous_rx_cb(wl_rx_cb);
    if (err != ESP_OK) {
        io_log("wlscan: set callback failed (err=%d)\r\n", (int)err);
        return err;
    }
    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK)
        io_log("wlscan: enable promiscuous failed (err=%d)\r\n", (int)err);
    return err;
}

static void wl_promis_stop(void)
{
    /* Detach the callback but keep promiscuous mode on: wifi_ctrl_init() needs
     * it enabled as a precondition for esp_wifi_80211_tx() (deauth/spam). */
    esp_wifi_set_promiscuous_rx_cb(NULL);
}

/* ── hop + report task ───────────────────────────────────────────────────── */

static void hop_channels(const uint8_t *plan, int n, uint32_t dwell,
                         int *tuned, int *n_new, int *n_upd, int *n_gone)
{
    for (int i = 0; i < n && !s_stop_requested; i++) {
        if (wl_tune(plan[i])) {
            (*tuned)++;
            vTaskDelay(pdMS_TO_TICKS(dwell));
            int64_t now = esp_timer_get_time();
            wl_flush(now, n_new, n_upd, n_gone);
            if (now - s_air_last_emit_us >= AIR_EMIT_INTERVAL_US)
                air_emit(now);
        } else if (plan == CH_5 && !s_5g_ok) {
            io_log("wlscan: 5GHz unavailable, continuing on 2.4GHz only\r\n");
            break;
        }
    }
}

static void wlscan_task(void *arg)
{
    (void)arg;
    int64_t started = esp_timer_get_time();
    s_cycle_count  = 0;
    s_total_unique = 0;
    s_cur_band     = 0;
    s_5g_ok        = true;

    portENTER_CRITICAL(&s_mux);
    memset(s_tbl, 0, sizeof(s_tbl));
    portEXIT_CRITICAL(&s_mux);

    memset((void *)&s_air, 0, sizeof(s_air));
    memset((void *)&s_air_snap, 0, sizeof(s_air_snap));
    s_air_last_emit_us = esp_timer_get_time();

    bool include5 = (s_mode != WLSCAN_MODE_FAST);
    uint32_t dwell;
    switch (s_mode) {
        case WLSCAN_MODE_DEEP:     dwell = 300; break;
        case WLSCAN_MODE_BALANCED: dwell = 150; break;
        default:                   dwell = 120; break;   /* FAST */
    }

    io_log("wlscan: START mode=%s dwell=%ums band=%s dur=%us\r\n",
           wifi_live_scan_mode_name(s_mode), (unsigned)dwell,
           include5 ? "2.4+5" : "2.4", (unsigned)s_duration_sec);

    if (wl_promis_start() != ESP_OK) {
        io_log("wlscan: cannot start capture\r\n");
        goto cleanup;
    }

    while (!s_stop_requested) {
        int tuned = 0, n_new = 0, n_upd = 0, n_gone = 0;

        hop_channels(CH_24, (int)(sizeof(CH_24) / sizeof(CH_24[0])),
                     dwell, &tuned, &n_new, &n_upd, &n_gone);

        if (include5 && s_5g_ok && !s_stop_requested) {
            hop_channels(CH_5, (int)(sizeof(CH_5) / sizeof(CH_5[0])),
                         dwell, &tuned, &n_new, &n_upd, &n_gone);
        }

        s_cycle_count++;
        io_log("wlscan: CYCLE %" PRIu32 " total=%d new=%d upd=%d gone=%d\r\n",
               s_cycle_count, wl_used_count(), n_new, n_upd, n_gone);

        /* Keep the nuke/deauth-all target list in sync with the radar. */
        wl_sync_targets();

        /* Safety: never hot-spin if the radio refused every channel. */
        if (tuned == 0 && !s_stop_requested)
            vTaskDelay(pdMS_TO_TICKS(500));

        if (s_duration_sec > 0) {
            uint32_t elapsed =
                (uint32_t)((esp_timer_get_time() - started) / 1000000);
            if (elapsed >= s_duration_sec) {
                io_log("wlscan: duration reached\r\n");
                break;
            }
        }
    }

cleanup:
    wl_promis_stop();
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    s_running = false;
    s_task = NULL;
    io_log("wlscan: STOPPED cycles=%" PRIu32 " unique=%" PRIu32 "\r\n",
           s_cycle_count, s_total_unique);
    vTaskDelete(NULL);
}

/* ── public API ──────────────────────────────────────────────────────────── */

esp_err_t wifi_live_scan_init(void)
{
    if (s_target_snapshot) return ESP_OK;

    s_target_snapshot = heap_caps_calloc(WLSCAN_MAX_APS,
                                         sizeof(*s_target_snapshot),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_target_snapshot) {
        io_log("wlscan: PSRAM target snapshot allocation failed\r\n");
        return ESP_ERR_NO_MEM;
    }

    s_running = false;
    s_stop_requested = false;
    return ESP_OK;
}

esp_err_t wifi_live_scan_start(wlscan_mode_t mode,
                               uint32_t interval_ms,
                               uint32_t duration_sec)
{
    (void)interval_ms;   /* continuous capture — no inter-cycle delay */

    if (!s_target_snapshot) return ESP_ERR_INVALID_STATE;

    /* A start request always means "give me a fresh scan". If a previous run is
     * still alive — the client re-opened the screen, reconnected over BLE, or an
     * earlier `wlscan stop` was dropped by a full command queue — tear it down
     * and restart instead of refusing. The client optimistically clears its AP
     * table on start and only repopulates from the NEW events a fresh run emits;
     * a bare refusal would leave it stuck on an empty, forever-"scanning" list
     * while this task keeps running but never re-announces the APs it already
     * reported. */
    if (s_running) {
        wifi_live_scan_stop();          /* sets stop flag, waits up to ~2 s */
    }
    /* Wait out any in-flight teardown: the task clears s_task last (after
     * s_running), so this guarantees we never spawn a second scan onto the same
     * radio and table. */
    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task != NULL) {
        io_log("wlscan: busy, try again\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (duration_sec > 3600) duration_sec = 3600;

    s_mode           = mode;
    s_duration_sec   = duration_sec;
    s_stop_requested = false;
    s_running        = true;

    BaseType_t ok = xTaskCreate(wlscan_task, "wlscan", 6144, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        /* Terminal line so the app clears its optimistic running state. */
        io_log("wlscan: STOPPED cycles=0 unique=0\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_live_scan_stop(void)
{
    if (!s_running) {
        io_log("wlscan: not running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

bool wifi_live_scan_is_running(void) { return s_running; }

const char *wifi_live_scan_mode_name(wlscan_mode_t mode)
{
    switch (mode) {
        case WLSCAN_MODE_FAST:     return "fast";
        case WLSCAN_MODE_BALANCED: return "balanced";
        case WLSCAN_MODE_DEEP:     return "deep";
        default:                   return "?";
    }
}

uint32_t wifi_live_scan_total_unique(void) { return s_total_unique; }
uint32_t wifi_live_scan_cycle_count(void)  { return s_cycle_count; }
