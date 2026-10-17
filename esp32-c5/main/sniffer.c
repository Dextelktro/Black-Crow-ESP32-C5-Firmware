#include "sniffer.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_err.h"

#include "io.h"
#include "targets.h"

static uint8_t s_filter_bssid[6];
static uint8_t s_channel;
static bool s_sweep_mode = false;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static void cb(void *buf, wifi_promiscuous_pkt_type_t type);

static esp_err_t promiscuous_start(void)
{
    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filt);
    if (err != ESP_OK) {
        io_log("sniff: set filter failed (err=%d)\r\n", (int)err);
        return err;
    }

    err = esp_wifi_set_promiscuous_rx_cb(cb);
    if (err != ESP_OK) {
        io_log("sniff: set callback failed (err=%d)\r\n", (int)err);
        return err;
    }

    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        io_log("sniff: enable promiscuous mode failed (err=%d)\r\n", (int)err);
    }
    return err;
}

static void promiscuous_stop(void)
{
    /* Detach the capture callback but KEEP promiscuous mode enabled.
     * wifi_ctrl_init() turns promiscuous on as a precondition for
     * esp_wifi_80211_tx(); disabling it here would silently break every
     * subsequent deauth / nuke / deauth-all / monitor until reboot. */
    esp_err_t err = esp_wifi_set_promiscuous_rx_cb(NULL);
    if (err != ESP_OK) {
        io_log("sniff: detach rx cb failed (err=%d)\r\n", (int)err);
    }
}

/* 802.11 frame control: bit positions
 * byte0 = | proto(2) | type(2) | subtype(4) |
 * byte1 = | toDS | fromDS | moreFrag | retry | pwrMgmt | moreData | protected | order |
 *
 * We use the toDS/fromDS bits to figure out where STA mac is.
 *   toDS=1, fromDS=0  → addr1=BSSID, addr2=STA, addr3=DA          (uplink)
 *   toDS=0, fromDS=1  → addr1=STA, addr2=BSSID, addr3=SA          (downlink)
 *   toDS=0, fromDS=0  → addr1=DA, addr2=SA, addr3=BSSID            (mgmt)
 */

static void cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
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
    uint8_t filter_bssid[6];
    uint8_t channel;
    bool sweep_mode;

    if (ftype == 0) {
        /* mgmt: BSSID is addr3, STA is addr2 (e.g., probe req from STA) */
        bssid = a3;
        sta   = a2;
    } else if (ftype == 2) {
        /* data */
        if (toDS && !fromDS)      { bssid = a1; sta = a2; }
        else if (!toDS && fromDS) { bssid = a2; sta = a1; }
        else                      { return; }
    } else {
        return;
    }

    portENTER_CRITICAL(&s_state_mux);
    sweep_mode = s_sweep_mode;
    channel = s_channel;
    memcpy(filter_bssid, s_filter_bssid, sizeof(filter_bssid));
    portEXIT_CRITICAL(&s_state_mux);

    if (!sweep_mode && memcmp(bssid, filter_bssid, 6) != 0) return;

    /* skip multicast/broadcast STAs */
    if (sta[0] & 0x01) return;
    /* skip our own mac mirrored back */
    if (memcmp(sta, bssid, 6) == 0) return;

    targets_add_sta(sta, bssid, channel, p->rx_ctrl.rssi);
}

int sniffer_run(uint8_t channel, const uint8_t *bssid, uint32_t seconds)
{
    if (!bssid) {
        io_log("sniff: missing BSSID\r\n");
        return -1;
    }
    if (seconds == 0) seconds = 5;
    if (seconds > 120) seconds = 120;

    portENTER_CRITICAL(&s_state_mux);
    memcpy(s_filter_bssid, bssid, sizeof(s_filter_bssid));
    s_channel = channel;
    s_sweep_mode = false;
    portEXIT_CRITICAL(&s_state_mux);

    int before = targets_sta_count();

    io_log("sniff: ch=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X  for %us\r\n",
           (unsigned)channel,
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
           (unsigned)seconds);

    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        io_log("sniff: set channel %u failed (err=%d)\r\n",
               (unsigned)channel, (int)err);
        return -1;
    }
    if (promiscuous_start() != ESP_OK) return -1;

    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    promiscuous_stop();

    int after = targets_sta_count();
    io_log("sniff: done, +%d STAs (total %d)\r\n", after - before, after);
    return after - before;
}

int sniffer_sweep(uint32_t seconds)
{
    if (seconds == 0) seconds = 13;
    if (seconds > 120) seconds = 120;

    io_log("dev: start — sweeping channels for %us\r\n", (unsigned)seconds);

    /* Prioritise non-overlapping 2.4 GHz channels, then fill the rest. */
    static const uint8_t channels[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    int n_ch = sizeof(channels) / sizeof(channels[0]);
    uint32_t dwell_ms = (seconds * 1000) / n_ch;
    if (dwell_ms < 500) dwell_ms = 500;

    portENTER_CRITICAL(&s_state_mux);
    s_sweep_mode = true;
    s_channel = channels[0];
    portEXIT_CRITICAL(&s_state_mux);
    esp_err_t err = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_state_mux);
        s_sweep_mode = false;
        portEXIT_CRITICAL(&s_state_mux);
        io_log("sniff: set channel %u failed (err=%d)\r\n",
               (unsigned)s_channel, (int)err);
        return -1;
    }
    if (promiscuous_start() != ESP_OK) {
        portENTER_CRITICAL(&s_state_mux);
        s_sweep_mode = false;
        portEXIT_CRITICAL(&s_state_mux);
        return -1;
    }

    bool channel_error = false;
    for (int i = 0; i < n_ch; i++) {
        if (i > 0) {
            portENTER_CRITICAL(&s_state_mux);
            s_channel = channels[i];
            portEXIT_CRITICAL(&s_state_mux);
            err = esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
            if (err != ESP_OK) {
                io_log("sniff: set channel %u failed (err=%d)\r\n",
                       (unsigned)s_channel, (int)err);
                channel_error = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    }

    promiscuous_stop();
    portENTER_CRITICAL(&s_state_mux);
    s_sweep_mode = false;
    portEXIT_CRITICAL(&s_state_mux);

    if (channel_error) return -1;

    int total = targets_sta_count();
    io_log("dev: %d devices found\r\n", total);
    return total;
}
