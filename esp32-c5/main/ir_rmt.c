#include "ir_rmt.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

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
#define IR_RX_BUFFER_COUNT 2
#define IR_RX_MIN_NS       2000         /* ignore < 2 us glitches      */
#define IR_RX_MAX_NS       12000000     /* > 12 ms gap ends a burst    */
#define IR_TX_END_GAP_US   4000         /* trailing idle after an odd mark */
#define IR_TX_WAIT_TIMEOUT_MS 3000      /* minimum wait bound for short TX frames */
#define IR_TX_WAIT_MARGIN_MS  1000      /* extra time for encoder/refill scheduling */

static const char *TAG = "ir_rmt";

static rmt_channel_handle_t s_tx_chan = NULL;
static rmt_channel_handle_t s_rx_chan = NULL;
static rmt_encoder_handle_t s_copy_enc = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;
static bool s_ready = false;

static int s_tx_gpio = -1;
static int s_rx_gpio = -1;

/* Carrier currently applied to the TX channel (so we only reconfigure
 * when a different protocol needs a different frequency). */
static uint32_t s_tx_carrier_hz = 0;
static float    s_tx_duty = 0.0f;
static bool     s_tx_enabled = false;
/* If a failed RMT stop cannot prove that s_tx_sym is no longer in use, refuse
 * future sends instead of overwriting a buffer that may still be on the wire. */
static bool     s_tx_faulted = false;

/* RX plumbing. */
static TaskHandle_t  s_rx_task = NULL;
static QueueHandle_t s_rx_queue = NULL;       /* ISR -> task: completed buffer */
static volatile bool s_rx_run = false;
static ir_rx_cb_t    s_rx_cb = NULL;
static void         *s_rx_ctx = NULL;
typedef struct {
    size_t num_symbols;
    uint8_t buffer_index;
} ir_rx_event_t;
static rmt_symbol_word_t *s_rx_symbols[IR_RX_BUFFER_COUNT];
static uint16_t      *s_rx_durations = NULL;
static esp_err_t      s_rx_stop_result = ESP_OK;
static volatile bool  s_rx_event_overflow = false;

static void rx_free_buffers(void)
{
    for (size_t i = 0; i < IR_RX_BUFFER_COUNT; i++) {
        free(s_rx_symbols[i]);
        s_rx_symbols[i] = NULL;
    }
    free(s_rx_durations);
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

/* Append one duration `v` (us) at logic `level` to the packed symbol buffer,
 * splitting durations longer than one 15-bit RMT field into same-level runs.
 * `*run` tracks the half-symbol slot. Returns false if `v` is 0 (invalid) or
 * the buffer is exhausted. */
static bool sym_push(size_t *run, uint8_t level, uint32_t v)
{
    if (v == 0) return false;
    while (v > 0) {
        if ((*run / 2) >= IR_TX_MAX_SYMBOLS) return false;
        uint32_t chunk = (v > 32767) ? 32767 : v;
        v -= chunk;
        size_t si = *run / 2;
        if ((*run & 1) == 0) {
            s_tx_sym[si].level0 = level;
            s_tx_sym[si].duration0 = (uint16_t)chunk;
        } else {
            s_tx_sym[si].level1 = level;
            s_tx_sym[si].duration1 = (uint16_t)chunk;
        }
        (*run)++;
    }
    return true;
}

/* Build symbols from an alternating duration list (mark first). Either `d16`
 * or `d32` is non-NULL. The frame is emitted `repeats` times (>=1), with
 * `gap_us` of trailing silence between consecutive parsed frames (never after
 * the last). RAW signals bypass this repeat path and retain their captured
 * inter-frame timing exactly.
 * Durations longer than one 15-bit RMT symbol field (32767 us @1 MHz) are
 * split into several same-level runs. Returns the symbol count; guarantees an
 * even run count so the final symbol never carries a 0-duration slot (which
 * the RMT treats as an end marker). */
static size_t build_symbols(const uint16_t *d16, const uint32_t *d32,
                            size_t count, int repeats, uint32_t gap_us)
{
    if (repeats < 1) repeats = 1;
    size_t run = 0;
    for (int r = 0; r < repeats; r++) {
        for (size_t i = 0; i < count; i++) {
            uint32_t v = d32 ? d32[i] : d16[i];
            uint8_t level = (i & 1) ? 0 : 1;   /* even index = mark (carrier on) */
            if (!sym_push(&run, level, v)) return 0;
        }
        if (r + 1 < repeats && gap_us > 0) {
            if (!sym_push(&run, 0, gap_us)) return 0;   /* inter-frame silence */
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

/* RMT runs at 1 MHz, so the built symbol durations are microseconds. Base the
 * wait deadline on the actual transaction rather than assuming every IR frame
 * is shorter than a fixed three-second ceiling. */
static int tx_wait_timeout_ms(size_t symbol_count)
{
    uint64_t duration_us = 0;
    for (size_t i = 0; i < symbol_count; i++) {
        duration_us += s_tx_sym[i].duration0;
        duration_us += s_tx_sym[i].duration1;
    }

    uint64_t timeout_ms = (duration_us + 999ULL) / 1000ULL + IR_TX_WAIT_MARGIN_MS;
    if (timeout_ms < IR_TX_WAIT_TIMEOUT_MS) timeout_ms = IR_TX_WAIT_TIMEOUT_MS;
    if (timeout_ms > INT_MAX) timeout_ms = INT_MAX;
    return (int)timeout_ms;
}

static esp_err_t tx_send(const uint16_t *d16, const uint32_t *d32, size_t count,
                         uint32_t carrier_hz, float duty,
                         int repeats, uint32_t gap_us)
{
    if (count == 0) return ESP_ERR_INVALID_ARG;
    if ((!d16 && !d32) || count > IR_RAW_MAX_TIMINGS) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_tx_mutex || xSemaphoreTake(s_tx_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;
    if (!s_ready) {
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (s_tx_faulted) {
        err = ESP_ERR_INVALID_STATE;
        io_log("ir: tx channel faulted; refusing to reuse the RMT payload buffer\r\n");
        goto done;
    }

    size_t nsym = build_symbols(d16, d32, count, repeats, gap_us);
    if (nsym == 0 || nsym > IR_TX_MAX_SYMBOLS) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    err = tx_apply_carrier(carrier_hz, duty);
    if (err != ESP_OK) goto done;

    rmt_transmit_config_t cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };
    err = rmt_transmit(s_tx_chan, s_copy_enc, s_tx_sym,
                       nsym * sizeof(rmt_symbol_word_t), &cfg);
    if (err == ESP_OK) {
        /* The copy encoder reads s_tx_sym incrementally; keep it immutable
         * until TX completes. Bounded wait: a stalled transfer returns
         * ESP_ERR_TIMEOUT instead of hanging the task (and the mutex) forever. */
        const int timeout_ms = tx_wait_timeout_ms(nsym);
        err = rmt_tx_wait_all_done(s_tx_chan, timeout_ms);
        if (err != ESP_OK) {
            /* A wait timeout does not cancel the transaction. Stop the channel
             * before releasing the mutex, because the copy encoder may still
             * be reading s_tx_sym. */
            esp_err_t stop_err = rmt_disable(s_tx_chan);
            if (stop_err == ESP_OK || stop_err == ESP_ERR_INVALID_STATE) {
                s_tx_enabled = false;
            } else {
                s_tx_faulted = true;
                io_log("ir: tx abort failed (%s); channel locked out\r\n",
                       esp_err_to_name(stop_err));
            }
            io_log("ir: tx wait failed (%s) after %d ms\r\n",
                   esp_err_to_name(err), timeout_ms);
        }
    }

done:
    xSemaphoreGive(s_tx_mutex);
    return err;
}

esp_err_t ir_rmt_send(const uint16_t *durations, size_t count,
                      uint32_t carrier_hz, float duty)
{
    if (!durations) return ESP_ERR_INVALID_ARG;
    return tx_send(durations, NULL, count, carrier_hz, duty, 1, 0);
}

esp_err_t ir_rmt_send_raw32(const uint32_t *durations, size_t count,
                            uint32_t carrier_hz, float duty)
{
    if (!durations) return ESP_ERR_INVALID_ARG;
    return tx_send(NULL, durations, count, carrier_hz, duty, 1, 0);
}

esp_err_t ir_rmt_send_repeat(const uint16_t *durations, size_t count,
                             uint32_t carrier_hz, float duty,
                             int repeats, uint32_t gap_us)
{
    if (!durations) return ESP_ERR_INVALID_ARG;
    return tx_send(durations, NULL, count, carrier_hz, duty, repeats, gap_us);
}

esp_err_t ir_rmt_send_raw32_repeat(const uint32_t *durations, size_t count,
                                   uint32_t carrier_hz, float duty,
                                   int repeats, uint32_t gap_us)
{
    if (!durations) return ESP_ERR_INVALID_ARG;
    return tx_send(NULL, durations, count, carrier_hz, duty, repeats, gap_us);
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
    ir_rx_event_t event = {
        .num_symbols = edata->num_symbols,
        .buffer_index = IR_RX_BUFFER_COUNT,
    };
    for (uint8_t i = 0; i < IR_RX_BUFFER_COUNT; i++) {
        if (edata->received_symbols == s_rx_symbols[i]) {
            event.buffer_index = i;
            break;
        }
    }
    if (xQueueSendFromISR(s_rx_queue, &event, &hp) != pdTRUE) {
        s_rx_event_overflow = true;
    }
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

static void rx_process_event(const ir_rx_event_t *event)
{
    if (event->buffer_index >= IR_RX_BUFFER_COUNT) {
        io_log("ir: rx received unknown buffer (%u)\r\n",
               (unsigned)event->buffer_index);
        s_rx_stop_result = ESP_ERR_INVALID_STATE;
        s_rx_run = false;
        return;
    }
    if (event->num_symbols > IR_RX_BUF_SYMBOLS) {
        io_log("ir: rx received invalid symbol count (%u)\r\n",
               (unsigned)event->num_symbols);
        s_rx_stop_result = ESP_ERR_INVALID_SIZE;
        s_rx_run = false;
        return;
    }
    if (event->num_symbols == IR_RX_BUF_SYMBOLS) {
        io_log("ir: rx buffer full; capture may be truncated\r\n");
    }
    if (event->num_symbols == 0 || !s_rx_cb) return;

    size_t dn = symbols_to_durations(s_rx_symbols[event->buffer_index],
                                     event->num_symbols, s_rx_durations,
                                     IR_RAW_MAX_TIMINGS);
    /* A real IR frame has at least a header + a few bits. */
    if (dn >= 4) s_rx_cb(s_rx_durations, dn, s_rx_ctx);
}

/* Report an RX event-queue overflow once and ask the task to stop. */
static void rx_flag_overflow(bool *reported)
{
    io_log("ir: rx event queue overflow\r\n");
    *reported = true;
    s_rx_stop_result = ESP_ERR_INVALID_STATE;
    s_rx_run = false;
}

static void rx_task(void *arg)
{
    (void)arg;
    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = IR_RX_MIN_NS,
        .signal_range_max_ns = IR_RX_MAX_NS,
    };

    esp_err_t err;
    bool overflow_reported = false;
    while (s_rx_run) {
        ir_rx_event_t event;
        if (xQueueReceive(s_rx_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_rx_event_overflow) {
                rx_flag_overflow(&overflow_reported);
            }

            if (event.buffer_index < IR_RX_BUFFER_COUNT && s_rx_run) {
                uint8_t next_buffer = event.buffer_index ^ 1U;
                err = rmt_receive(s_rx_chan, s_rx_symbols[next_buffer],
                                  IR_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t),
                                  &rcfg);
                if (err != ESP_OK) {
                    io_log("ir: rx re-arm failed (%d)\r\n", (int)err);
                    s_rx_stop_result = err;
                    s_rx_run = false;
                }
            }
            rx_process_event(&event);
        } else if (s_rx_event_overflow) {
            rx_flag_overflow(&overflow_reported);
        }
    }

    err = rmt_disable(s_rx_chan);
    if (err != ESP_OK) {
        io_log("ir: rx disable failed (%d)\r\n", (int)err);
        s_rx_stop_result = err;
    } else {
        ir_rx_event_t event;
        while (xQueueReceive(s_rx_queue, &event, 0) == pdTRUE) {
            rx_process_event(&event);
        }
        rx_free_buffers();
    }
    if (s_rx_event_overflow && !overflow_reported) {
        io_log("ir: rx event queue overflow\r\n");
        s_rx_stop_result = ESP_ERR_INVALID_STATE;
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
    if (s_rx_symbols[0] || s_rx_symbols[1] || s_rx_durations) {
        esp_err_t cleanup_err = rmt_disable(s_rx_chan);
        if (cleanup_err != ESP_OK) {
            io_log("ir: rx recovery disable failed (%d)\r\n",
                   (int)cleanup_err);
            return cleanup_err;
        }
        rx_free_buffers();
    }
    for (size_t i = 0; i < IR_RX_BUFFER_COUNT; i++) {
        s_rx_symbols[i] = heap_caps_calloc(
            IR_RX_BUF_SYMBOLS, sizeof(rmt_symbol_word_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_rx_durations = heap_caps_calloc(IR_RAW_MAX_TIMINGS,
                                     sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rx_symbols[0] || !s_rx_symbols[1] || !s_rx_durations) {
        io_log("ir: rx buffer alloc failed (internal heap)\r\n");
        rx_free_buffers();
        return ESP_ERR_NO_MEM;
    }
    xQueueReset(s_rx_queue);
    s_rx_event_overflow = false;
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
    err = rmt_receive(s_rx_chan, s_rx_symbols[0],
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
    io_log("ir: rx stop requested; active partial burst may be discarded\r\n");
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

static esp_err_t ir_rmt_init_abort(esp_err_t cause)
{
    /* Best-effort teardown: release every handle we hold even if one delete
     * fails (a failed delete must not leak the remaining handles), and always
     * leave the static state fully reset so a later re-init starts clean. */
    if (s_rx_chan) {
        esp_err_t err = rmt_del_channel(s_rx_chan);
        if (err != ESP_OK) io_log("ir: rx init cleanup failed (%d)\r\n", (int)err);
        s_rx_chan = NULL;
    }
    if (s_rx_queue) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    if (s_copy_enc) {
        esp_err_t err = rmt_del_encoder(s_copy_enc);
        if (err != ESP_OK) io_log("ir: encoder init cleanup failed (%d)\r\n", (int)err);
        s_copy_enc = NULL;
    }
    if (s_tx_chan) {
        esp_err_t err = rmt_del_channel(s_tx_chan);
        if (err != ESP_OK) io_log("ir: tx init cleanup failed (%d)\r\n", (int)err);
        s_tx_chan = NULL;
    }
    if (s_tx_mutex) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }
    s_tx_gpio = -1;
    s_rx_gpio = -1;
    s_ready = false;
    s_tx_faulted = false;
    return cause;
}

esp_err_t ir_rmt_init(int tx_gpio, int rx_gpio)
{
    if (s_ready) return ESP_OK;
    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (!s_tx_mutex) return ESP_ERR_NO_MEM;
    }
    s_tx_gpio = tx_gpio;
    s_rx_gpio = rx_gpio;

    rmt_tx_channel_config_t txc = {
        .gpio_num = tx_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_TX_MEM_SYMBOLS,
        .trans_queue_depth = 1,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    esp_err_t err = rmt_new_tx_channel(&txc, &s_tx_chan);
    if (err != ESP_OK) {
        io_log("ir: tx channel failed (%d)\r\n", err);
        return ir_rmt_init_abort(err);
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &s_copy_enc);
    if (err != ESP_OK) {
        io_log("ir: copy encoder failed (%d)\r\n", err);
        return ir_rmt_init_abort(err);
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
        return ir_rmt_init_abort(err);
    }

    s_rx_queue = xQueueCreate(2, sizeof(ir_rx_event_t));
    if (!s_rx_queue) {
        io_log("ir: rx alloc failed\r\n");
        return ir_rmt_init_abort(ESP_ERR_NO_MEM);
    }

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    err = rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL);
    if (err != ESP_OK) {
        io_log("ir: rx callbacks failed (%d)\r\n", err);
        return ir_rmt_init_abort(err);
    }

    s_ready = true;
    ESP_LOGI(TAG, "IR RMT ready tx=%d rx=%d", tx_gpio, rx_gpio);
    return ESP_OK;
}
