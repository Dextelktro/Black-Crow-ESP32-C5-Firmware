#include "wifi_spam.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_rom_sys.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "io.h"

static const char *TAG = "wspam";

/* NVS */
#define WSPAM_NVS_NAMESPACE  "wspam"
#define WSPAM_NVS_KEY_COUNT  "count"
#define WSPAM_CUSTOM_MAX     32
#define WSPAM_SSID_LEN       32

/* How often to emit the live SSID feed line (ms). */
#define WSPAM_SSID_FEED_MS   150

/* ============================================================
 * SSID Lists
 * ============================================================ */
static const char *FUNNY_SSIDS[] = {
    "FBI Surveillance Van #42",
    "Pretty Fly for a WiFi",
    "Tell My WiFi Love Her",
    "Drop It Like It's Hotspot",
    "Abraham Linksys",
    "Martin Router King",
    "Wu-Tang Lan",
    "It Burns When IP",
    "The Promised Lan",
    "Silence of the LANs",
    "Get Off My LAN",
    "Nacho WiFi",
    "I Can Haz Internet",
    "Lan Solo",
    "Bag of Holding",
    "The LAN Before Time",
    "WiFi Art Thou",
    "One Does Not Simply",
    "Router? I Hardly Know Her",
    "Virus.exe",
    "Loading...",
    "NSA Listening Post",
    "CIA Van",
    "MI6 Field Office",
    "Hack Me If You Can",
    "Connect and Die",
    "Definitely Not a Trap",
    "Free WiFi (Not Really)",
    "SSID Not Found",
    "Pikachu's WiFi",
    "Dumbledore's Army",
    "The Shire",
    "Winterfell",
    "Hogwarts Great Hall",
    "Gotham City",
    "Narnia",
    "Neverland",
    "Atlantis",
    "El Dorado",
    "Skynet",
};
#define FUNNY_N (sizeof(FUNNY_SSIDS) / sizeof(FUNNY_SSIDS[0]))

static const char *RICKROLL_SSIDS[] = {
    "Never Gonna Give You Up",
    "Never Gonna Let You Down",
    "Never Gonna Run Around",
    "And Desert You",
    "Never Gonna Make You Cry",
    "Never Gonna Say Goodbye",
    "Never Gonna Tell a Lie",
    "And Hurt You",
    "We're No Strangers to Love",
    "You Know the Rules",
};
#define RICKROLL_N (sizeof(RICKROLL_SSIDS) / sizeof(RICKROLL_SSIDS[0]))

/* Custom SSID buffer (NVS-persisted) */
static char     s_custom_ssids[WSPAM_CUSTOM_MAX][WSPAM_SSID_LEN + 1];
static uint32_t s_custom_count = 0;

/* ============================================================
 * Channel lists
 * ============================================================ */
static const uint8_t CH_24GHZ[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
#define CH_24GHZ_N (sizeof(CH_24GHZ) / sizeof(CH_24GHZ[0]))

static const uint8_t CH_5GHZ[] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165,
};
#define CH_5GHZ_N (sizeof(CH_5GHZ) / sizeof(CH_5GHZ[0]))

/* ============================================================
 * State
 * ============================================================ */
static TaskHandle_t  s_task = NULL;
static volatile bool s_running = false;
static volatile bool s_stop_requested = false;

static wspam_mode_t s_mode = WSPAM_MODE_FUNNY;
static wspam_rate_t s_rate = WSPAM_RATE_AGGRESSIVE;
static bool         s_band_5g = true;
static uint32_t     s_duration_s = 0;
static int64_t      s_started_us = 0;

static volatile uint32_t s_total_sent = 0;
static volatile uint32_t s_current_channel = 1;
static uint32_t          s_ssid_index = 0;
static uint32_t          s_ch_index = 0;
static uint16_t          s_seq = 0;

/* ============================================================
 * NVS helpers
 * ============================================================ */
static esp_err_t nvs_load_custom(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WSPAM_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        s_custom_count = 0;
        return ESP_OK;
    }

    uint32_t count = 0;
    err = nvs_get_u32(h, WSPAM_NVS_KEY_COUNT, &count);
    if (err != ESP_OK || count > WSPAM_CUSTOM_MAX) {
        nvs_close(h);
        s_custom_count = 0;
        return ESP_OK;
    }

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ssid_%" PRIu32, i);
        size_t len = WSPAM_SSID_LEN + 1;
        err = nvs_get_str(h, key, s_custom_ssids[loaded], &len);
        if (err == ESP_OK && len > 0) {
            s_custom_ssids[loaded][WSPAM_SSID_LEN] = '\0';
            loaded++;
        }
    }
    nvs_close(h);
    s_custom_count = loaded;
    ESP_LOGI(TAG, "NVS: loaded %u custom SSIDs", (unsigned)loaded);
    return ESP_OK;
}

static esp_err_t nvs_save_custom(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WSPAM_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    /* Erase every possible key first, then rewrite. */
    for (uint32_t i = 0; i < WSPAM_CUSTOM_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ssid_%" PRIu32, i);
        nvs_erase_key(h, key);  /* ignore error if absent */
    }

    err = nvs_set_u32(h, WSPAM_NVS_KEY_COUNT, s_custom_count);
    if (err != ESP_OK) { nvs_close(h); return err; }

    for (uint32_t i = 0; i < s_custom_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "ssid_%" PRIu32, i);
        err = nvs_set_str(h, key, s_custom_ssids[i]);
        if (err != ESP_OK) { nvs_close(h); return err; }
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ============================================================
 * Beacon frame builder
 * ============================================================ */
static int build_beacon(uint8_t *buf, size_t buf_size,
                        const char *ssid, uint8_t ssid_len,
                        uint8_t channel, const uint8_t *mac)
{
    if (ssid_len > WSPAM_SSID_LEN) ssid_len = WSPAM_SSID_LEN;
    size_t total = 73 + ssid_len;
    if (total > buf_size) return -1;

    memset(buf, 0, total);
    size_t i = 0;

    buf[i++] = 0x80;  /* Frame control: mgmt beacon */
    buf[i++] = 0x00;
    buf[i++] = 0x00;  /* Duration */
    buf[i++] = 0x00;
    memset(buf + i, 0xFF, 6); i += 6;  /* DA: broadcast */
    memcpy(buf + i, mac, 6); i += 6;   /* SA */
    memcpy(buf + i, mac, 6); i += 6;   /* BSSID */

    s_seq = (s_seq + 1) & 0x0FFF;
    uint16_t seq = s_seq << 4;
    buf[i++] = seq & 0xFF;
    buf[i++] = (seq >> 8) & 0xFF;

    uint64_t tsf = (uint64_t)esp_timer_get_time();
    for (int k = 0; k < 8; k++) {
        buf[i++] = (uint8_t)(tsf >> (k * 8));
    }

    buf[i++] = 0x64;  /* Beacon interval 100 TU */
    buf[i++] = 0x00;
    buf[i++] = 0x11;  /* Capability: ESS + Privacy */
    buf[i++] = 0x04;  /* Short slot */

    buf[i++] = 0x00;  /* SSID IE tag */
    buf[i++] = ssid_len;
    memcpy(buf + i, ssid, ssid_len);
    i += ssid_len;

    buf[i++] = 0x01;  /* Supported Rates */
    buf[i++] = 0x08;
    buf[i++] = 0x82; buf[i++] = 0x84; buf[i++] = 0x8B; buf[i++] = 0x96;
    buf[i++] = 0x0C; buf[i++] = 0x12; buf[i++] = 0x18; buf[i++] = 0x24;

    buf[i++] = 0x03;  /* DS Param Set */
    buf[i++] = 0x01;
    buf[i++] = channel;

    buf[i++] = 0x30;  /* RSN IE */
    buf[i++] = 0x14;
    buf[i++] = 0x01; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x0F; buf[i++] = 0xAC; buf[i++] = 0x04;
    buf[i++] = 0x01; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x0F; buf[i++] = 0xAC; buf[i++] = 0x04;
    buf[i++] = 0x01; buf[i++] = 0x00;
    buf[i++] = 0x00; buf[i++] = 0x0F; buf[i++] = 0xAC; buf[i++] = 0x02;
    buf[i++] = 0x00; buf[i++] = 0x00;

    return (int)i;
}

/* ============================================================
 * Helpers
 * ============================================================ */
static void gen_random_mac(uint8_t *mac)
{
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(esp_random() & 0xFF);
    mac[0] = (mac[0] & 0xFC) | 0x02;
}

static void gen_random_ssid(char *out, size_t out_size)
{
    size_t len = 8 + (esp_random() % 13);
    if (len >= out_size) len = out_size - 1;
    static const char cs[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < len; i++) {
        out[i] = cs[esp_random() % (sizeof(cs) - 1)];
    }
    out[len] = '\0';
}

static void get_current_ssid(char *out, size_t out_size, uint8_t *len_out)
{
    const char *src = NULL;
    switch (s_mode) {
        case WSPAM_MODE_FUNNY:
            src = FUNNY_SSIDS[s_ssid_index % FUNNY_N];
            break;
        case WSPAM_MODE_RICKROLL:
            src = RICKROLL_SSIDS[s_ssid_index % RICKROLL_N];
            break;
        case WSPAM_MODE_ALL: {
            uint32_t pick = esp_random() % 3;
            if (pick == 0) {
                src = FUNNY_SSIDS[esp_random() % FUNNY_N];
            } else if (pick == 1) {
                src = RICKROLL_SSIDS[esp_random() % RICKROLL_N];
            } else {
                gen_random_ssid(out, out_size);
                *len_out = (uint8_t)strlen(out);
                return;
            }
            break;
        }
        case WSPAM_MODE_CUSTOM:
            if (s_custom_count > 0) {
                src = s_custom_ssids[s_ssid_index % s_custom_count];
            } else {
                src = "(empty)";
            }
            break;
        case WSPAM_MODE_RANDOM:
        default:
            gen_random_ssid(out, out_size);
            *len_out = (uint8_t)strlen(out);
            return;
    }
    size_t l = strlen(src);
    if (l >= out_size) l = out_size - 1;
    memcpy(out, src, l);
    out[l] = '\0';
    *len_out = (uint8_t)l;
}

static uint32_t rate_delay_ms(void)
{
    switch (s_rate) {
        case WSPAM_RATE_AGGRESSIVE: return 10;
        case WSPAM_RATE_STEALTH:    return 100;
        case WSPAM_RATE_NORMAL:
        default:                    return 20;
    }
}

static uint32_t total_ssids_for_mode(void)
{
    switch (s_mode) {
        case WSPAM_MODE_FUNNY:    return FUNNY_N;
        case WSPAM_MODE_RICKROLL: return RICKROLL_N;
        case WSPAM_MODE_ALL:      return FUNNY_N + RICKROLL_N;
        case WSPAM_MODE_CUSTOM:   return s_custom_count;
        case WSPAM_MODE_RANDOM:   return 0;
        default:                   return 0;
    }
}

/* ============================================================
 * Task
 * ============================================================ */
static void wspam_task(void *arg)
{
    (void)arg;
    s_started_us = esp_timer_get_time();
    s_total_sent = 0;
    s_ssid_index = 0;
    s_ch_index = 0;

    /* Defensive: promiscuous ON (TX precondition) + band AUTO. */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    vTaskDelay(pdMS_TO_TICKS(50));

    io_log("wspam: START mode=%s rate=%s delay=%ums band=%s dur=%us\r\n",
           wifi_spam_mode_name(s_mode), wifi_spam_rate_name(s_rate),
           (unsigned)rate_delay_ms(), s_band_5g ? "dual" : "2.4G",
           (unsigned)s_duration_s);

    uint8_t frame[128];
    char ssid_buf[WSPAM_SSID_LEN + 1];
    uint8_t ssid_len;
    uint8_t mac[6];
    uint32_t last_stat_sec = 0;
    uint32_t last_stat_total = 0;
    uint32_t last_feed_ms = 0;
    uint8_t last_ch = 0;
    bool first_tx_warned = false;

    while (!s_stop_requested) {
        if (s_duration_s > 0) {
            uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_started_us)
                                            / 1000000);
            if (elapsed >= s_duration_s) break;
        }

        uint8_t ch;
        if (!s_band_5g) {
            ch = CH_24GHZ[s_ch_index % CH_24GHZ_N];
        } else {
            uint32_t total_ch = CH_24GHZ_N + CH_5GHZ_N;
            uint32_t idx = s_ch_index % total_ch;
            ch = (idx < CH_24GHZ_N) ? CH_24GHZ[idx] : CH_5GHZ[idx - CH_24GHZ_N];
        }
        s_ch_index++;

        esp_err_t ch_err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        if (ch_err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        s_current_channel = ch;

        bool cross_band = (ch > 14) != (last_ch > 14);
        esp_rom_delay_us(cross_band ? 3000 : 1500);
        last_ch = ch;

        get_current_ssid(ssid_buf, sizeof(ssid_buf), &ssid_len);
        gen_random_mac(mac);

        int flen = build_beacon(frame, sizeof(frame), ssid_buf,
                                ssid_len, ch, mac);
        if (flen < 0) {
            vTaskDelay(pdMS_TO_TICKS(rate_delay_ms()));
            continue;
        }

        esp_err_t tx = esp_wifi_80211_tx(WIFI_IF_STA, frame, flen, false);
        if (tx == ESP_OK) {
            s_total_sent++;
        } else if (!first_tx_warned) {
            first_tx_warned = true;
            ESP_LOGW(TAG, "80211_tx first failure rc=%d", tx);
        }

        /* Live SSID feed — throttled so the app shows names in real time. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_feed_ms >= WSPAM_SSID_FEED_MS) {
            last_feed_ms = now_ms;
            io_log("wspam: SSID ch=%u name=\"%s\"\r\n",
                   (unsigned)ch, ssid_buf);
        }

        s_ssid_index++;
        vTaskDelay(pdMS_TO_TICKS(rate_delay_ms()));

        uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us)
                                    / 1000000);
        if (sec - last_stat_sec >= 2) {
            uint32_t dsec = sec - last_stat_sec;
            uint32_t fps = dsec > 0 ? (s_total_sent - last_stat_total) / dsec : 0;
            last_stat_sec = sec;
            last_stat_total = s_total_sent;
            io_log("wspam: STAT %us total=%" PRIu32 " fps=%" PRIu32
                   " ch=%u ssid=\"%s\"\r\n",
                   (unsigned)sec, s_total_sent, fps, (unsigned)ch, ssid_buf);
        }
    }

    uint32_t total_time = (uint32_t)((esp_timer_get_time() - s_started_us)
                                       / 1000000);
    io_log("wspam: STOPPED total=%" PRIu32 " took=%us\r\n",
           s_total_sent, (unsigned)total_time);

    s_task = NULL;
    s_running = false;
    vTaskDelete(NULL);
}

/* ============================================================
 * Public API
 * ============================================================ */
esp_err_t wifi_spam_init(void)
{
    nvs_load_custom();
    s_running = false;
    s_stop_requested = false;
    return ESP_OK;
}

esp_err_t wifi_spam_start(wspam_mode_t mode, wspam_rate_t rate,
                          bool band_5g, uint32_t duration_s)
{
    if (mode >= WSPAM_MODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (rate > WSPAM_RATE_STEALTH) return ESP_ERR_INVALID_ARG;

    if (s_running || s_task != NULL) {
        io_log("wspam: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == WSPAM_MODE_CUSTOM && s_custom_count == 0) {
        io_log("wspam: custom mode but no SSIDs — use `wspam add <ssid>`\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (duration_s > 3600) duration_s = 3600;

    s_mode = mode;
    s_rate = rate;
    s_band_5g = band_5g;
    s_duration_s = duration_s;
    s_stop_requested = false;
    s_running = true;

    BaseType_t ok = xTaskCreate(wspam_task, "wspam", 5120, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_spam_stop(void)
{
    if (!s_running) {
        io_log("wspam: not running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = true;

    int max_wait = 50;
    for (int i = 0; i < max_wait && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "wspam: task did not exit in %dms", max_wait * 10);
    }
    return ESP_OK;
}

bool wifi_spam_is_running(void) { return s_running; }

const char *wifi_spam_mode_name(wspam_mode_t mode)
{
    switch (mode) {
        case WSPAM_MODE_FUNNY:    return "funny";
        case WSPAM_MODE_RICKROLL: return "rickroll";
        case WSPAM_MODE_RANDOM:   return "random";
        case WSPAM_MODE_ALL:      return "all";
        case WSPAM_MODE_CUSTOM:   return "custom";
        default:                   return "?";
    }
}

const char *wifi_spam_rate_name(wspam_rate_t rate)
{
    switch (rate) {
        case WSPAM_RATE_AGGRESSIVE: return "aggressive";
        case WSPAM_RATE_NORMAL:     return "normal";
        case WSPAM_RATE_STEALTH:    return "stealth";
        default:                     return "?";
    }
}

wspam_mode_t wifi_spam_current_mode(void)      { return s_mode; }
wspam_rate_t wifi_spam_current_rate(void)      { return s_rate; }
bool         wifi_spam_current_dual_band(void) { return s_band_5g; }
uint32_t     wifi_spam_total_sent(void)        { return s_total_sent; }
uint32_t     wifi_spam_current_channel(void)   { return s_current_channel; }
uint32_t     wifi_spam_ssid_count(void)        { return total_ssids_for_mode(); }

uint32_t wifi_spam_elapsed_sec(void)
{
    if (s_started_us == 0) return 0;
    return (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
}

esp_err_t wifi_spam_custom_add(const char *ssid)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    size_t l = strlen(ssid);
    if (l > WSPAM_SSID_LEN) return ESP_ERR_INVALID_SIZE;
    if (s_custom_count >= WSPAM_CUSTOM_MAX) return ESP_ERR_NO_MEM;

    for (uint32_t i = 0; i < s_custom_count; i++) {
        if (strcmp(s_custom_ssids[i], ssid) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    strncpy(s_custom_ssids[s_custom_count], ssid, WSPAM_SSID_LEN);
    s_custom_ssids[s_custom_count][WSPAM_SSID_LEN] = '\0';
    s_custom_count++;

    esp_err_t err = nvs_save_custom();
    if (err != ESP_OK) {
        s_custom_count--;
        s_custom_ssids[s_custom_count][0] = '\0';
        ESP_LOGE(TAG, "NVS save failed: %d", err);
        return err;
    }
    return ESP_OK;
}

esp_err_t wifi_spam_custom_clear(void)
{
    uint32_t prev = s_custom_count;
    s_custom_count = 0;
    esp_err_t err = nvs_save_custom();
    if (err != ESP_OK) {
        s_custom_count = prev;
        ESP_LOGE(TAG, "NVS clear failed: %d", err);
        return err;
    }
    return ESP_OK;
}

uint32_t wifi_spam_custom_count(void) { return s_custom_count; }

esp_err_t wifi_spam_custom_get(uint32_t idx, char *out, size_t out_size)
{
    if (idx >= s_custom_count) return ESP_ERR_INVALID_ARG;
    if (!out || out_size == 0) return ESP_ERR_INVALID_ARG;
    strncpy(out, s_custom_ssids[idx], out_size - 1);
    out[out_size - 1] = '\0';
    return ESP_OK;
}
