#include "nrf24_sniffer.h"
#include "nrf24_driver.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "io.h"

static const char *TAG = "nrf_sniff";

/* FIX C3: task untuk replay async */
typedef struct {
    uint32_t idx;
    uint32_t count;
    uint32_t delay_us;
    bool     active;
} replay_args_t;

static replay_args_t s_replay_args = {0};
static TaskHandle_t  s_replay_task = NULL;

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static volatile bool     s_stop_requested = false;

static nrf24_sniff_packet_t *s_packets;
static uint32_t s_count = 0;
static uint8_t  s_channel = 76;
static uint32_t s_duration_s = 0;
static int64_t  s_started_us = 0;

static uint32_t fnv_hash(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

esp_err_t nrf24_sniffer_init(void)
{
    if (s_lock && s_packets) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_packets = heap_caps_calloc(NRF24_SNIFF_MAX_PACKETS,
                                 sizeof(*s_packets),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_packets) {
        ESP_LOGE(TAG, "PSRAM packet buffer allocation failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_count = 0;
    ESP_LOGI(TAG, "sniffer init OK");
    return ESP_OK;
}

static bool is_duplicate(uint32_t hash)
{
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_packets[i].hash == hash) return true;
    }
    return false;
}

static void sniffer_task(void *arg)
{
    (void)arg;
    s_started_us = esp_timer_get_time();

    /* Radio sudah di-claim oleh nrf24_sniffer_start() */

    io_log("nrf_sniff: START ch=%u dur=%us\r\n",
           (unsigned)s_channel, (unsigned)s_duration_s);

    nrf24_enter_promiscuous();
    nrf24_set_channel(s_channel);

    uint32_t last_stat = 0;
    uint32_t dropped = 0;
    uint32_t rx_burst = 0;   /* FIX M2: counter yield periodik */

    while (!s_stop_requested) {
        if (s_duration_s > 0) {
            uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
            if (elapsed >= s_duration_s) break;
        }

        if (!nrf24_rx_available()) {
            vTaskDelay(1);
            continue;
        }

        uint8_t buf[NRF24_SNIFF_PAYLOAD_LEN];
        int len = nrf24_rx(buf, sizeof(buf));
        if (len <= 0) continue;

        uint32_t hash = fnv_hash(buf, (size_t)len);

        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (is_duplicate(hash)) {
            if (s_lock) xSemaphoreGive(s_lock);
            continue;
        }
        if (s_count >= NRF24_SNIFF_MAX_PACKETS) {
            dropped++;
            if (s_lock) xSemaphoreGive(s_lock);
            continue;
        }

        nrf24_sniff_packet_t *p = &s_packets[s_count];
        memcpy(p->payload, buf, (size_t)len);
        p->len = (uint8_t)len;
        p->channel = s_channel;
        p->rssi = 0;
        p->timestamp_ms = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000);
        p->hash = hash;
        s_count++;

        uint32_t idx = s_count - 1;
        if (s_lock) xSemaphoreGive(s_lock);

        char hex[3 * 16 + 1];
        int hexlen = (len < 16) ? len : 16;
        for (int i = 0; i < hexlen; i++) snprintf(hex + i * 3, 4, "%02X ", buf[i]);
        hex[hexlen * 3] = '\0';

        io_log("nrf_sniff: PKT idx=%u len=%d data=%s%s\r\n",
               (unsigned)idx, len, hex, (len > 16) ? "..." : "");

        uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
        if (sec - last_stat >= 5) {
            last_stat = sec;
            io_log("nrf_sniff: STAT %us captured=%u dropped=%u\r\n",
                   (unsigned)sec, (unsigned)s_count, (unsigned)dropped);
        }

        /* FIX M2: yield periodik kalau FIFO penuh dengan noise */
        if ((++rx_burst % 10) == 0) {
            vTaskDelay(1);
        }
    }

    nrf24_exit_promiscuous();

    io_log("nrf_sniff: STOPPED total=%u dropped=%u\r\n",
           (unsigned)s_count, (unsigned)dropped);

    nrf24_radio_release();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nrf24_sniffer_start(uint8_t channel, uint32_t duration_s)
{
    if (!s_lock || !s_packets) return ESP_ERR_INVALID_STATE;
    if (!nrf24_is_present()) return ESP_ERR_INVALID_STATE;
    if (channel > 125) return ESP_ERR_INVALID_ARG;
    if (s_running) return ESP_ERR_INVALID_STATE;

    /* FIX H1: claim atomic */
    if (!nrf24_radio_claim("sniff")) {
        io_log("nrf_sniff: radio busy\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (duration_s > 600) duration_s = 600;
    s_channel = channel;
    s_duration_s = duration_s;
    s_stop_requested = false;
    s_running = true;

    BaseType_t ok = xTaskCreate(sniffer_task, "nrf_sniff", 6144, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        nrf24_radio_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nrf24_sniffer_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

bool nrf24_sniffer_is_running(void) { return s_running; }

void nrf24_sniffer_dump(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    io_log("nrf_sniff: LS count=%u\r\n", (unsigned)s_count);
    for (uint32_t i = 0; i < s_count; i++) {
        const nrf24_sniff_packet_t *p = &s_packets[i];
        char hex[3 * 16 + 1] = {0};
        int hexlen = (p->len < 16) ? p->len : 16;
        for (int j = 0; j < hexlen; j++) snprintf(hex + j * 3, 4, "%02X ", p->payload[j]);
        io_log("nrf_sniff: LS idx=%u ch=%u len=%u t=%ums data=%s%s\r\n",
               (unsigned)i, (unsigned)p->channel, (unsigned)p->len,
               (unsigned)p->timestamp_ms, hex,
               (p->len > 16) ? "..." : "");
    }
    io_log("nrf_sniff: LS end\r\n");
    xSemaphoreGive(s_lock);
}

uint32_t nrf24_sniffer_count(void)
{
    if (!s_lock) return 0;
    uint32_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t nrf24_sniffer_get(uint32_t idx, nrf24_sniff_packet_t *out)
{
    if (!s_lock || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx >= s_count) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out, &s_packets[idx], sizeof(*out));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void nrf24_sniffer_clear(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_lock);
    io_log("nrf_sniff: cleared\r\n");
}

/* FIX C3: worker task untuk replay — tidak block CLI */
static void replay_task(void *arg)
{
    (void)arg;
    uint32_t idx      = s_replay_args.idx;
    uint32_t count    = s_replay_args.count;
    uint32_t delay_us = s_replay_args.delay_us;

    nrf24_sniff_packet_t pkt;
    int ok_count = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx >= s_count) {
        xSemaphoreGive(s_lock);
        io_log("nrf_sniff: replay invalid idx\r\n");
        goto exit;
    }
    memcpy(&pkt, &s_packets[idx], sizeof(pkt));
    xSemaphoreGive(s_lock);

    nrf24_set_channel(pkt.channel);
    if (nrf24_is_rx_mode()) nrf24_enter_tx();

    for (uint32_t i = 0; i < count; i++) {
        esp_err_t err = nrf24_tx(pkt.payload, pkt.len);
        if (err != ESP_OK) {
            io_log("nrf_sniff: replay fail at %u err=%d\r\n",
                   (unsigned)i, err);
            break;
        }
        ok_count++;

        /* FIX C3: pakai vTaskDelay untuk delay >= 1ms, yield ke idle */
        if (delay_us >= 1000) {
            vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
        } else if (delay_us > 0) {
            esp_rom_delay_us(delay_us);
        }

        /* Extra yield tiap 10 tx untuk cegah WDT */
        if ((i % 10) == 9) vTaskDelay(1);
    }

    io_log("nrf_sniff: replay done (%d/%u)\r\n",
           ok_count, (unsigned)count);

exit:
    nrf24_standby();
    nrf24_radio_release();
    s_replay_args.active = false;
    s_replay_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nrf24_sniffer_replay(uint32_t idx, uint32_t count, uint32_t delay_us)
{
    if (!nrf24_is_present()) return ESP_ERR_INVALID_STATE;
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    if (s_replay_args.active) {
        io_log("nrf_sniff: replay already active\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    /* FIX H1: claim atomic sebelum bikin task */
    if (!nrf24_radio_claim("replay")) {
        io_log("nrf_sniff: radio busy\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (count == 0) count = 1;
    if (count > 1000) count = 1000;
    if (delay_us > 1000000) delay_us = 1000000;

    s_replay_args.idx      = idx;
    s_replay_args.count    = count;
    s_replay_args.delay_us = delay_us;
    s_replay_args.active   = true;

    io_log("nrf_sniff: replay start idx=%u count=%u delay=%uus\r\n",
           (unsigned)idx, (unsigned)count, (unsigned)delay_us);

    BaseType_t ok = xTaskCreate(replay_task, "nrf_replay", 4096,
                                NULL, 3, &s_replay_task);
    if (ok != pdPASS) {
        s_replay_args.active = false;
        nrf24_radio_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
