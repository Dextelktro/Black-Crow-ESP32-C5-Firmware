#include "ir_rmt.h"

#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "io.h"

#define IR_RESOLUTION_HZ   1000000      /* 1 tick = 1 us */
/* ESP32-C5 RMT: 4 channels, 48 words each (2 TX / 2 RX capable). The LED
 * strip owns one TX channel, so IR TX + IR RX each take a single 48-word
 * block (no cross-channel borrowing). Non-DMA ping-pong receive still
 * captures frames far longer than one block into the user buffer below. */
#define IR_TX_MEM_SYMBOLS  48
#define IR_RX_MEM_SYMBOLS  48
#define IR_RX_BUF_SYMBOLS  512          /* up to 1024 edges per burst */
#define IR_RX_MIN_NS       2000         /* ignore < 2 us glitches      */
#define IR_RX_MAX_NS       12000000     /* > 12 ms gap ends a burst    */
#define IR_TX_END_GAP_US   4000         /* trailing idle after an odd mark */

static const char *TAG = "ir_rmt";

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_channel_handle_t s_rx_chan = NULL;
static rmt_encoder_handle_t s_copy_enc = NULL;
static bool s_ready = false;

static int s_tx_gpio = -1;
static int s_rx_gpio = -1;

/* Carrier currently applied to the TX channel (so we only reconfigure
 * when a different protocol needs a different frequency). */
static uint32_t s_tx_carrier_hz = 0;
static float    s_tx_duty = 0.0f;
static bool     s_tx_enabled = false;

/* RX plumbing. */
static TaskHandle_t  s_rx_task = NULL;
static QueueHandle_t s_rx_queue = NULL;       /* ISR -> task: num symbols  */
static volatile bool s_rx_run = false;
static ir_rx_cb_t    s_rx_cb = NULL;
static void         *s_rx_ctx = NULL;
static rmt_symbol_word_t *s_rx_symbols = NULL;
static uint16_t      *s_rx_durations = NULL;
static esp_err_t      s_rx_stop_result = ESP_OK;

static void rx_free_buffers(void)
{
    free(s_rx_symbols);
    free(s_rx_durations);
    s_rx_symbols = NULL;
    s_rx_durations = NULL;
}

/* ---------------------------------------------------------------- */

bool ir_rmt_ready(void) { return s_ready; }

/* ------------------------------ TX ------------------------------ */

static esp_err_t tx_apply_carrier(uint32_t carrier_hz, float duty)
{
    if (carrier_hz == 0) carrier_hz = 38000;
    if (duty <= 0.0f || duty >= 1.0f) duty = 0.33f;

    if (s_tx_carrier_hz == carrier_hz && s_tx_duty == duty && s_tx_enabled) {
        return ESP_OK;
    }
    if (s_tx_enabled) {
        esp_err_t err = rmt_disable(s_tx_chan);
        if (err != ESP_OK) {
            io_log("ir: tx disable failed (%d)\r\n", err);
            return err;
        }
        s_tx_enabled = false;
    }
    rmt_carrier_config_t carrier = {
        .frequency_hz = carrier_hz,
        .duty_cycle   = duty,
        .flags.polarity_active_low = false,
        .flags.always_on = false,
    };
    esp_err_t err = rmt_apply_carrier(s_tx_chan, &carrier);
    if (err != ESP_OK) {
        io_log("ir: apply carrier failed (%d)\r\n", err);
        return err;
    }
    s_tx_carrier_hz = carrier_hz;
    s_tx_duty = duty;
    err = rmt_enable(s_tx_chan);
    if (err != ESP_OK) {
        io_log("ir: tx enable failed (%d)\r\n", err);
        return err;
    }
    s_tx_enabled = true;
    return ESP_OK;
}

/* Static TX symbol buffer — no per-send heap allocation (TX is driven by a
 * single task, so this is never re-entered). Sized for the longest supplied
 * universal signal after long-duration splitting. */
#define IR_TX_MAX_SYMBOLS 512
static rmt_symbol_word_t s_tx_sym[IR_TX_MAX_SYMBOLS];

/* Build symbols from an alternating duration list (mark first). Either
 * `d16` or `d32` is non-NULL. Durations longer than one 15-bit RMT symbol
 * field (32767 us @1 MHz) are split into several same-level runs. Returns
 * the symbol count; guarantees an even run count so the final symbol never
 * carries a 0-duration slot (which the RMT treats as an end marker). */
static size_t build_symbols(const uint16_t *d16, const uint32_t *d32,
                            size_t count)
{
    size_t run = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t v = d32 ? d32[i] : d16[i];
        if (v == 0) return 0;
        uint8_t level = (i & 1) ? 0 : 1;   /* even index = mark (carrier on) */
        while (v > 0) {
            if ((run / 2) >= IR_TX_MAX_SYMBOLS) {
                return 0;
            }
            uint32_t chunk = (v > 32767) ? 32767 : v;
            v -= chunk;
            size_t si = run / 2;
            if ((run & 1) == 0) {
                s_tx_sym[si].level0 = level;
                s_tx_sym[si].duration0 = (uint16_t)chunk;
            } else {
                s_tx_sym[si].level1 = level;
                s_tx_sym[si].duration1 = (uint16_t)chunk;
            }
            run++;
        }
    }
    if (run & 1) {
        if ((run / 2) >= IR_TX_MAX_SYMBOLS) return 0;
        size_t si = run / 2;
        s_tx_sym[si].level1 = 0;
        s_tx_sym[si].duration1 = IR_TX_END_GAP_US;
        run++;
    }
    return run / 2;
}

static esp_err_t tx_send(const uint16_t *d16, const uint32_t *d32, size_t count,
                         uint32_t carrier_hz, float duty)
{
    if (!s_ready || count == 0) return ESP_ERR_INVALID_ARG;
    if ((!d16 && !d32) || count > IR_RAW_MAX_TIMINGS) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t nsym = build_symbols(d16, d32, count);
    if (nsym == 0 || nsym > IR_TX_MAX_SYMBOLS) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = tx_apply_carrier(carrier_hz, duty);
    if (err != ESP_OK) return err;

    rmt_transmit_config_t cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };
    err = rmt_transmit(s_tx_chan, s_copy_enc, s_tx_sym,
                       nsym * sizeof(rmt_symbol_word_t), &cfg);
    if (err == ESP_OK) {
        /* A multi-frame raw signal can run ~1.5 s; wait generously. */
        err = rmt_tx_wait_all_done(s_tx_chan, 3000);
    }
    return err;
}

esp_err_t ir_rmt_send(const uint16_t *durations, size_t count,
                      uint32_t carrier_hz, float duty)
{
    if (!durations) return ESP_ERR_INVALID_ARG;
    return tx_send(durations, NULL, count, carrier_hz, duty);
}

esp_err_t ir_rmt_send_raw32(const uint32_t *durations, size_t count,
                            uint32_t carrier_hz, float duty)
{
    if (!durations) return ESP_ERR_INVALID_ARG;
    return tx_send(NULL, durations, count, carrier_hz, duty);
}

/* ------------------------------ RX ------------------------------ */

/* The callback lives in flash by default (zero IRAM cost, so the IR feature
 * can never contribute to an IRAM overflow). It is promoted to IRAM only
 * when the RX ISR is made cache-safe via CONFIG_RMT_RX_ISR_CACHE_SAFE. */
#if CONFIG_RMT_RX_ISR_CACHE_SAFE
#define IR_RX_CB_ATTR IRAM_ATTR
#else
#define IR_RX_CB_ATTR
#endif

static bool IR_RX_CB_ATTR rx_done_cb(rmt_channel_handle_t chan,
                                     const rmt_rx_done_event_data_t *edata,
                                     void *user_data)
{
    (void)chan; (void)user_data;
    BaseType_t hp = pdFALSE;
    size_t num = edata->num_symbols;
    xQueueSendFromISR(s_rx_queue, &num, &hp);
    return hp == pdTRUE;
}

static size_t symbols_to_durations(const rmt_symbol_word_t *sym, size_t num,
                                   uint16_t *out, size_t out_cap)
{
    /* Flatten to (level,duration) order, mark first, trailing idle removed. */
    size_t n = 0;
    bool started = false;
    for (size_t i = 0; i < num && n < out_cap; i++) {
        /* duration0 */
        if (sym[i].duration0 > 0) {
            if (!started) {
                if (sym[i].level0 != 1) {
                    /* leading space (idle) — skip until first mark */
                } else {
                    started = true;
                    out[n++] = sym[i].duration0;
                }
            } else if (n < out_cap) {
                out[n++] = sym[i].duration0;
            }
        }
        if (n >= out_cap) break;
        /* duration1 */
        if (sym[i].duration1 > 0) {
            if (started && n < out_cap) {
                out[n++] = sym[i].duration1;
            }
        }
    }
    /* Trim any trailing spaces so the list ends on a mark. Each stored
     * element alternates mark,space,... starting with a mark, so an
     * even count means the last element is a space. */
    if ((n % 2) == 0 && n > 0) n--;
    return n;
}

static void rx_task(void *arg)
{
    (void)arg;
    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = IR_RX_MIN_NS,
        .signal_range_max_ns = IR_RX_MAX_NS,
    };

    esp_err_t err;
    while (s_rx_run) {
        size_t num = 0;
        if (xQueueReceive(s_rx_queue, &num, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (num > 0 && s_rx_cb) {
                size_t dn = symbols_to_durations(s_rx_symbols, num,
                                                 s_rx_durations,
                                                 IR_RAW_MAX_TIMINGS);
                /* A real IR frame has at least a header + a few bits. */
                if (dn >= 4) {
                    s_rx_cb(s_rx_durations, dn, s_rx_ctx);
                }
            }
            /* Re-arm for the next burst. */
            if (s_rx_run) {
                err = rmt_receive(s_rx_chan, s_rx_symbols,
                                  IR_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t),
                                  &rcfg);
                if (err != ESP_OK) {
                    io_log("ir: rx re-arm failed (%d)\r\n", (int)err);
                    s_rx_stop_result = err;
                    s_rx_run = false;
                }
            }
        }
    }

    err = rmt_disable(s_rx_chan);
    if (err != ESP_OK) {
        io_log("ir: rx disable failed (%d)\r\n", (int)err);
        s_rx_stop_result = err;
    } else {
        rx_free_buffers();
    }
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t ir_rmt_rx_start(ir_rx_cb_t cb, void *ctx)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_rx_run || s_rx_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rx_symbols || s_rx_durations) {
        esp_err_t cleanup_err = rmt_disable(s_rx_chan);
        if (cleanup_err != ESP_OK) {
            io_log("ir: rx recovery disable failed (%d)\r\n",
                   (int)cleanup_err);
            return cleanup_err;
        }
        rx_free_buffers();
    }
    s_rx_symbols = heap_caps_calloc(IR_RX_BUF_SYMBOLS,
                                   sizeof(rmt_symbol_word_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_rx_durations = heap_caps_calloc(IR_RAW_MAX_TIMINGS,
                                     sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rx_symbols || !s_rx_durations) {
        io_log("ir: rx buffer alloc failed (internal heap)\r\n");
        free(s_rx_symbols);
        free(s_rx_durations);
        s_rx_symbols = NULL;
        s_rx_durations = NULL;
        return ESP_ERR_NO_MEM;
    }
    xQueueReset(s_rx_queue);
    s_rx_cb = cb;
    s_rx_ctx = ctx;
    s_rx_stop_result = ESP_OK;
    esp_err_t err = rmt_enable(s_rx_chan);
    if (err != ESP_OK) {
        io_log("ir: rx enable failed (%d)\r\n", (int)err);
        rx_free_buffers();
        return err;
    }

    s_rx_run = true;
    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = IR_RX_MIN_NS,
        .signal_range_max_ns = IR_RX_MAX_NS,
    };
    err = rmt_receive(s_rx_chan, s_rx_symbols,
                      IR_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t), &rcfg);
    if (err != ESP_OK) {
        io_log("ir: rx arm failed (%d)\r\n", (int)err);
        s_rx_run = false;
        esp_err_t disable_err = rmt_disable(s_rx_chan);
        s_rx_stop_result = disable_err == ESP_OK ? err : disable_err;
        if (disable_err == ESP_OK) rx_free_buffers();
        return s_rx_stop_result;
    }

    s_rx_run = true;
    if (xTaskCreate(rx_task, "ir_rx", 6144, NULL, 10, &s_rx_task) != pdPASS) {
        s_rx_run = false;
        s_rx_task = NULL;
        err = rmt_disable(s_rx_chan);
        s_rx_stop_result = err == ESP_OK ? ESP_ERR_NO_MEM : err;
        if (err == ESP_OK) rx_free_buffers();
        return s_rx_stop_result;
    }
    return ESP_OK;
}

esp_err_t ir_rmt_rx_stop(void)
{
    if (!s_rx_run && !s_rx_task) return s_rx_stop_result;
    s_rx_run = false;
    for (int i = 0; i < 50 && s_rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rx_task != NULL) {
        io_log("ir: rx stop timed out\r\n");
        return ESP_ERR_TIMEOUT;
    }
    return s_rx_stop_result;
}

bool ir_rmt_rx_active(void) { return s_rx_run; }

/* ----------------------------- init ----------------------------- */

esp_err_t ir_rmt_init(int tx_gpio, int rx_gpio)
{
    if (s_ready) return ESP_OK;
    s_tx_gpio = tx_gpio;
    s_rx_gpio = rx_gpio;

    rmt_tx_channel_config_t txc = {
        .gpio_num = tx_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_TX_MEM_SYMBOLS,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    esp_err_t err = rmt_new_tx_channel(&txc, &s_tx_chan);
    if (err != ESP_OK) {
        io_log("ir: tx channel failed (%d)\r\n", err);
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &s_copy_enc);
    if (err != ESP_OK) {
        io_log("ir: copy encoder failed (%d)\r\n", err);
        return err;
    }

    rmt_rx_channel_config_t rxc = {
        .gpio_num = rx_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_MEM_SYMBOLS,
        .flags.invert_in = true,     /* TSOP output is active-low */
        .flags.with_dma = false,
    };
    err = rmt_new_rx_channel(&rxc, &s_rx_chan);
    if (err != ESP_OK) {
        io_log("ir: rx channel failed (%d)\r\n", err);
        return err;
    }

    s_rx_queue = xQueueCreate(4, sizeof(size_t));
    if (!s_rx_queue) {
        io_log("ir: rx alloc failed\r\n");
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    err = rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL);
    if (err != ESP_OK) {
        io_log("ir: rx callbacks failed (%d)\r\n", err);
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "IR RMT ready tx=%d rx=%d", tx_gpio, rx_gpio);
    return ESP_OK;
}
