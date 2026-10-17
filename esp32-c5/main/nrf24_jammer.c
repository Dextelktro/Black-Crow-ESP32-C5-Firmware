#include "nrf24_jammer.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "io.h"

static const char *TAG = "nrf_jam";

/* ============================================================
 * FIX C1: helper yield untuk cegah Task WDT reset.
 * Dipanggil di dalam loop utama jammer + loop CW/flood/reactive.
 * ============================================================ */
#define JAM_YIELD_INTERVAL_MS  10
static int64_t s_last_yield_us = 0;

static inline void jam_yield_if_needed(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_yield_us >= JAM_YIELD_INTERVAL_MS * 1000) {
        s_last_yield_us = now;
        vTaskDelay(1);   /* tick 1ms — biarkan idle task jalan */
    }
}

static const uint8_t CH_WIFI_24GHZ[] = {2,3,4,5,6,7,8,9,10,11,12,13,14};
static const uint8_t CH_BLE_ADV[]     = {2, 26, 80};
static const uint8_t CH_BLE_DATA[] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
    20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,
};
static const uint8_t CH_BT_CLASSIC[] = {
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
    38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    72, 73, 74, 75, 76, 77, 78, 79, 80
};
static const uint8_t CH_ZIGBEE[] = {5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80};
static const uint8_t CH_DRONE_FHSS[] = {
    0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,
    100,105,110,115,120,125,
};
static const uint8_t CH_USB_DONGLE[] = {40, 50, 60, 70, 80};

static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static volatile bool     s_stop_requested = false;

static jam_config_t s_cfg;
static uint32_t     s_duration_s = 0;
static int64_t      s_started_us = 0;
static volatile uint32_t s_frame_count = 0;
static volatile uint8_t  s_current_ch = 0;
static volatile bool     s_waterfall_mode = false;

void nrf24_jammer_default_config(jam_target_t target, jam_config_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->target     = target;
    out->strategy   = JAM_STRAT_CW;
    out->rate       = NRF24_RATE_2M;
    out->pa         = NRF24_PA_MAX;
    out->dwell_us   = 400;
    out->burst      = 1;
    out->random_hop = false;
}

const char *nrf24_jammer_target_name(jam_target_t t)
{
    switch (t) {
        case JAM_TARGET_FULL:       return "full";
        case JAM_TARGET_WIFI:       return "wifi";
        case JAM_TARGET_BLE_ADV:    return "ble_adv";
        case JAM_TARGET_BLE_DATA:   return "ble_data";
        case JAM_TARGET_BT_CLASSIC: return "bt_classic";
        case JAM_TARGET_ZIGBEE:     return "zigbee";
        case JAM_TARGET_DRONE:      return "drone";
        case JAM_TARGET_USB_DONGLE: return "usb_dongle";
        default:                     return "?";
    }
}

const char *nrf24_jammer_strategy_name(jam_strategy_t s)
{
    switch (s) {
        case JAM_STRAT_CW:       return "cw";
        case JAM_STRAT_FLOOD:    return "flood";
        case JAM_STRAT_REACTIVE: return "reactive";
        default:                  return "?";
    }
}

static bool get_channel_list(jam_target_t t, const uint8_t **list, size_t *n)
{
    switch (t) {
        case JAM_TARGET_WIFI:       *list = CH_WIFI_24GHZ; *n = sizeof(CH_WIFI_24GHZ); return true;
        case JAM_TARGET_BLE_ADV:    *list = CH_BLE_ADV;    *n = sizeof(CH_BLE_ADV);    return true;
        case JAM_TARGET_BLE_DATA:   *list = CH_BLE_DATA;   *n = sizeof(CH_BLE_DATA);   return true;
        case JAM_TARGET_BT_CLASSIC: *list = CH_BT_CLASSIC; *n = sizeof(CH_BT_CLASSIC); return true;
        case JAM_TARGET_ZIGBEE:     *list = CH_ZIGBEE;     *n = sizeof(CH_ZIGBEE);     return true;
        case JAM_TARGET_DRONE:      *list = CH_DRONE_FHSS; *n = sizeof(CH_DRONE_FHSS); return true;
        case JAM_TARGET_USB_DONGLE: *list = CH_USB_DONGLE; *n = sizeof(CH_USB_DONGLE); return true;
        case JAM_TARGET_FULL:
        default:                     *list = NULL; *n = 126; return false;
    }
}

static void do_cw_jam(uint8_t ch)
{
    if (nrf24_get_channel() != ch) nrf24_set_channel(ch);
    nrf24_start_carrier();
    if (s_cfg.dwell_us >= 100) esp_rom_delay_us(s_cfg.dwell_us);
    nrf24_stop_carrier();
    s_frame_count++;
    jam_yield_if_needed();   /* FIX C1 */
}

static void do_flood_jam(uint8_t ch)
{
    if (nrf24_get_channel() != ch) nrf24_set_channel(ch);
    if (nrf24_is_rx_mode()) nrf24_enter_tx();

    uint8_t noise[32];
    for (int i = 0; i < 32; i++) noise[i] = (uint8_t)(esp_random() & 0xFF);
    for (int b = 0; b < s_cfg.burst; b++) {
        nrf24_tx_no_ack(noise, 32);
        s_frame_count++;
    }
    if (s_cfg.dwell_us >= 50) esp_rom_delay_us(s_cfg.dwell_us);
    jam_yield_if_needed();   /* FIX C1 */
}

static void do_reactive_jam(uint8_t ch)
{
    if (nrf24_get_channel() != ch) nrf24_set_channel(ch);
    if (!nrf24_is_rx_mode()) nrf24_enter_rx();
    esp_rom_delay_us(NRF24_CH_SETTLE_US);

    if (nrf24_get_rpd()) {
        uint8_t noise[32];
        for (int i = 0; i < 32; i++) noise[i] = (uint8_t)(esp_random() & 0xFF);
        nrf24_enter_tx();
        nrf24_tx_no_ack(noise, 32);
        s_frame_count++;
        nrf24_enter_rx();
    }
    jam_yield_if_needed();   /* FIX C1 */
}

static void jam_task(void *arg)
{
    (void)arg;
    s_started_us = esp_timer_get_time();
    s_frame_count = 0;
    s_last_yield_us = esp_timer_get_time();

    /* Radio sudah di-claim oleh jammer_start() sebelum task dibuat */

    nrf24_set_pa_level(s_cfg.pa);
    nrf24_set_data_rate(s_cfg.rate);

    const uint8_t *ch_list = NULL;
    size_t ch_n = 0;
    bool has_list = get_channel_list(s_cfg.target, &ch_list, &ch_n);

    io_log("nrfjam: START target=%s strategy=%s rate=%d pa=%d dwell=%uus burst=%u dur=%us wf=%d\r\n",
           nrf24_jammer_target_name(s_cfg.target),
           nrf24_jammer_strategy_name(s_cfg.strategy),
           (int)s_cfg.rate, (int)s_cfg.pa,
           (unsigned)s_cfg.dwell_us, (unsigned)s_cfg.burst,
           (unsigned)s_duration_s, (int)s_waterfall_mode);

    uint32_t last_stat = 0;
    uint32_t ch_idx = 0;

    while (!s_stop_requested) {
        if (s_duration_s > 0) {
            uint32_t elapsed = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
            if (elapsed >= s_duration_s) break;
        }

        if (s_waterfall_mode) {
            nrf24_enter_rx();
            uint8_t peak_ch = 0;
            int best = 0;

            for (int ch = 0; ch < 126 && !s_stop_requested; ch++) {
                nrf24_set_channel((uint8_t)ch);
                esp_rom_delay_us(200);
                uint8_t v = 0;
                for (int s = 0; s < 2; s++) {
                    if (nrf24_get_rpd()) v++;
                    esp_rom_delay_us(30);
                }
                if (v > best) { best = v; peak_ch = (uint8_t)ch; }
            }

            if (best > 0) {
                io_log("nrfjam: wf peak ch=%u strength=%d\r\n",
                       (unsigned)peak_ch, best);
                uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
                while (!s_stop_requested &&
                       ((uint32_t)(esp_timer_get_time() / 1000) - t0) < 500) {
                    if (s_cfg.strategy == JAM_STRAT_CW) do_cw_jam(peak_ch);
                    else if (s_cfg.strategy == JAM_STRAT_FLOOD) do_flood_jam(peak_ch);
                    else do_reactive_jam(peak_ch);
                    jam_yield_if_needed();   /* FIX C1: double safety */
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            /* Emit progress so the app telemetry (frames/elapsed) updates in
             * waterfall mode too, not only the wf-peak line. */
            uint32_t wsec =
                (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
            if (wsec - last_stat >= 2) {
                last_stat = wsec;
                io_log("nrfjam: %us ch=%u frames=%u\r\n",
                       (unsigned)wsec, (unsigned)peak_ch,
                       (unsigned)s_frame_count);
            }
            continue;
        }

        uint8_t ch;
        if (s_cfg.random_hop) {
            ch = (uint8_t)(esp_random() % 126);
        } else if (has_list) {
            ch = ch_list[ch_idx % ch_n];
            ch_idx++;
        } else {
            ch = (uint8_t)(ch_idx % 126);
            ch_idx++;
        }
        s_current_ch = ch;

        switch (s_cfg.strategy) {
            case JAM_STRAT_CW:       do_cw_jam(ch);       break;
            case JAM_STRAT_FLOOD:    do_flood_jam(ch);    break;
            case JAM_STRAT_REACTIVE: do_reactive_jam(ch); break;
        }

        uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
        if (sec - last_stat >= 2) {
            last_stat = sec;
            io_log("nrfjam: %us ch=%u frames=%u\r\n",
                   (unsigned)sec, (unsigned)ch, (unsigned)s_frame_count);
        }
    }

    if (nrf24_is_carrier_on()) nrf24_stop_carrier();
    nrf24_standby();

    uint32_t total_time = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
    io_log("nrfjam: STOPPED frames=%u took=%us\r\n",
           (unsigned)s_frame_count, (unsigned)total_time);

    nrf24_radio_release();

    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nrf24_jammer_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t jammer_start(const jam_config_t *cfg, uint32_t duration_s, bool waterfall)
{
    if (!nrf24_is_present()) return ESP_ERR_INVALID_STATE;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (s_running) {
        io_log("nrfjam: already running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->target >= JAM_TARGET_COUNT) return ESP_ERR_INVALID_ARG;
    if (cfg->strategy > JAM_STRAT_REACTIVE) return ESP_ERR_INVALID_ARG;
    if (duration_s > 3600) duration_s = 3600;

    /* FIX H1: claim radio atomic di sini, gagal-cepat kalau sudah dipakai */
    if (!nrf24_radio_claim("jam")) {
        io_log("nrfjam: radio busy (already claimed)\r\n");
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    s_duration_s = duration_s;
    s_waterfall_mode = waterfall;
    s_stop_requested = false;
    s_running = true;

    BaseType_t ok = xTaskCreate(jam_task, "nrf_jam", 4096, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        nrf24_radio_release();   /* rollback claim */
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nrf24_jammer_start_manual(const jam_config_t *cfg, uint32_t duration_s)
{
    return jammer_start(cfg, duration_s, false);
}

esp_err_t nrf24_jammer_start_auto(jam_target_t target, uint32_t duration_s)
{
    if (target >= JAM_TARGET_COUNT) return ESP_ERR_INVALID_ARG;

    jam_config_t cfg;
    nrf24_jammer_default_config(target, &cfg);

    switch (target) {
        case JAM_TARGET_BLE_ADV:
            cfg.strategy = JAM_STRAT_REACTIVE;
            cfg.dwell_us = 1000;
            break;
        case JAM_TARGET_WIFI:
            cfg.strategy = JAM_STRAT_CW;
            cfg.dwell_us = 500;
            break;
        case JAM_TARGET_BT_CLASSIC:
            /* BT hop 625µs — dwell 700µs cover 1 full hop + margin */
            cfg.strategy = JAM_STRAT_CW;
            cfg.dwell_us = 700;
            cfg.burst = 1;
            break;
        case JAM_TARGET_DRONE:
            /* Drone hop 100–1000 Hz — dwell 500µs compromise */
            cfg.strategy = JAM_STRAT_CW;
            cfg.dwell_us = 500;
            cfg.burst = 1;
            break;
        case JAM_TARGET_ZIGBEE:
        case JAM_TARGET_BLE_DATA:
            cfg.strategy = JAM_STRAT_FLOOD;
            cfg.dwell_us = 400;
            cfg.burst = 3;
            break;
        case JAM_TARGET_USB_DONGLE:
            cfg.strategy = JAM_STRAT_CW;
            cfg.dwell_us = 800;
            break;
        case JAM_TARGET_FULL:
        default:
            cfg.strategy = JAM_STRAT_CW;
            cfg.dwell_us = 200;
            cfg.rate = NRF24_RATE_2M;
            break;
    }

    return jammer_start(&cfg, duration_s, false);
}

esp_err_t nrf24_jammer_start_waterfall(uint32_t duration_s)
{
    jam_config_t cfg;
    nrf24_jammer_default_config(JAM_TARGET_FULL, &cfg);
    cfg.strategy = JAM_STRAT_CW;
    cfg.dwell_us = 200;
    return jammer_start(&cfg, duration_s, true);
}

esp_err_t nrf24_jammer_stop(void)
{
    if (!s_running) {
        io_log("nrfjam: not running\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_task != NULL) ESP_LOGW(TAG, "jam task stuck");
    return ESP_OK;
}

bool nrf24_jammer_is_running(void) { return s_running; }
