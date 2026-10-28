#include "subghz_capture.h"

#include <string.h>
#include <stdlib.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/rmt_rx.h"
#include "esp_timer.h"

#include "board_config.h"
#include "io.h"

#define SUBGHZ_GDO0_GPIO        BOARD_CC1101_GDO0_GPIO
#define SUBGHZ_RX_RESOLUTION_HZ 1000000     /* 1 tick = 1 us (see static assert) */
#define SUBGHZ_RX_MEM_SYMBOLS   48          /* channel memory block (>=48, even) */
#define SUBGHZ_RX_BUF_SYMBOLS   512         /* up to 1024 edges per burst */
#define SUBGHZ_RX_BUFFER_COUNT  2
#define SUBGHZ_RX_MIN_NS        3000       /* C5 APB filter limit is < 3187 ns */
#define SUBGHZ_RX_MIN_EDGES     16           /* shorter frames are noise fragments */

/* Software noise de-speckle (Flipper's te_short, lib/subghz/protocols/raw.c).
 * The ESP32-C5 RMT glitch filter (signal_range_min_ns) is capped at ~3187 ns
 * (RMT_LL_MAX_FILTER_VALUE / clk_MHz), so it cannot reject the 3..50 us runts an
 * OOK slicer emits on noise. Flipper drops pulses <= 50 us in SOFTWARE instead;
 * we do the same here. A short pulse of the OPPOSITE level to its neighbours is
 * absorbed into the surrounding level (it was a demod glitch, not a real edge),
 * which collapses noise bursts into far fewer edges so they fail the MIN_EDGES
 * and decoder checks. 50 us is safely below the shortest real protocol element
 * (CAME Te = 320 us), so no valid signal is touched. */
#define SUBGHZ_RX_DESPECKLE_US  50

/* --- Capture boundary: two SEPARATE concepts (Stage 7 Part C) ---------------
 * The RMT peripheral ends a reception when any single level (HIGH or LOW)
 * exceeds signal_range_max_ns (ESP-IDF v5.5 semantics). That hardware limit is
 * the MAX RECORDABLE LEVEL: it must be LONGER than the longest valid in-frame
 * pulse so no frame is ever cut mid-data. It is NOT, by itself, the definition
 * of a frame boundary.
 *
 * SUBGHZ_RX_MAX_NS (10 ms) — max recordable level + coarse reception end. Every
 * supported protocol's longest in-frame pulse is well under it (the widest is
 * Clemsa's te_long, 2695 us), and every inter-frame gap we rely on the hardware
 * to split is above it (>= ~11 ms). Kept at 10 ms so the ten existing protocols
 * segment exactly as before (zero change to their behaviour).
 *
 * SUBGHZ_FRAME_GAP_US (6 ms) — SOFTWARE frame-end. Within one reception, a low
 * of at least this length marks a frame boundary, and the reception is split
 * into frames there. This resolves short-gap protocols (e.g. SMC5326 ~7.2 ms,
 * Dooya ~9.2 ms) whose inter-frame gap is BELOW the 10 ms hardware limit: the
 * gap is recorded inside the reception and the software separates the frames.
 *
 * Invariant (must hold for every supported protocol):
 *   longest in-frame pulse  <  SUBGHZ_FRAME_GAP_US  <  shortest inter-frame gap
 *   (Clemsa 2695 us)        <  6000 us              <  (SMC5326 ~7200 us)
 * A low in (FRAME_GAP, RX_MAX] stays inside one hardware reception and is split
 * by software; a low > RX_MAX ends the reception in hardware. Both paths yield
 * the same per-frame callbacks. */
#define SUBGHZ_RX_MAX_NS        10000000    /* 10 ms: max recordable level */
#define SUBGHZ_FRAME_GAP_US     6000        /* software frame-end gap */

/* The symbol->microsecond conversion below uses the raw RMT tick count as a
 * microsecond value directly. That is correct ONLY while 1 tick == 1 us, i.e.
 * the channel resolution is exactly 1 MHz. If the resolution ever changes, the
 * conversion (and every stored duration) must change with it — this assert
 * makes that coupling a compile error instead of a silent unit bug. The channel
 * uses RMT_CLK_SRC_DEFAULT, matching the existing IR RX channel on this C5 so
 * the shared RMT group clock/prescaler stays consistent. */
_Static_assert(SUBGHZ_RX_RESOLUTION_HZ == 1000000,
               "duration conversion assumes 1 RMT tick == 1 microsecond");

/* Single source of truth for the receive window (glitch filter + burst-end
 * threshold), shared by the initial arm and every ping-pong re-arm so they can
 * never drift apart. */
static const rmt_receive_config_t s_rcfg = {
    .signal_range_min_ns = SUBGHZ_RX_MIN_NS,
    .signal_range_max_ns = SUBGHZ_RX_MAX_NS,
};

static rmt_channel_handle_t s_rx_chan;
static QueueHandle_t  s_rx_queue;
static TaskHandle_t   s_rx_task;
static volatile bool  s_rx_run;
static subghz_capture_cb_t s_cb;
static void          *s_ctx;

typedef struct {
    size_t  num_symbols;
    uint8_t buffer_index;
} subghz_rx_event_t;

static rmt_symbol_word_t *s_rx_symbols[SUBGHZ_RX_BUFFER_COUNT];  /* internal RAM */
static int32_t           *s_durations;                          /* PSRAM        */
static esp_err_t          s_stop_result;
static volatile bool      s_event_overflow;
static subghz_capture_stats_t s_stats;

/* The done callback sits in flash (zero IRAM cost) unless the RMT RX ISR is
 * made cache-safe, exactly like the IR front-end. */
#if CONFIG_RMT_RX_ISR_CACHE_SAFE
#define SUBGHZ_RX_CB_ATTR IRAM_ATTR
#else
#define SUBGHZ_RX_CB_ATTR
#endif

static void free_buffers(void)
{
    for (size_t i = 0; i < SUBGHZ_RX_BUFFER_COUNT; i++) {
        free(s_rx_symbols[i]);
        s_rx_symbols[i] = NULL;
    }
    heap_caps_free(s_durations);
    s_durations = NULL;
}

bool subghz_capture_active(void) { return s_rx_run; }

static bool SUBGHZ_RX_CB_ATTR rx_done_cb(rmt_channel_handle_t chan,
                                         const rmt_rx_done_event_data_t *edata,
                                         void *user_data)
{
    (void)chan; (void)user_data;
    BaseType_t hp = pdFALSE;
    subghz_rx_event_t event = {
        .num_symbols = edata->num_symbols,
        .buffer_index = SUBGHZ_RX_BUFFER_COUNT,
    };
    for (uint8_t i = 0; i < SUBGHZ_RX_BUFFER_COUNT; i++) {
        if (edata->received_symbols == s_rx_symbols[i]) {
            event.buffer_index = i;
            break;
        }
    }
    if (xQueueSendFromISR(s_rx_queue, &event, &hp) != pdTRUE) {
        s_event_overflow = true;
    }
    return hp == pdTRUE;
}

/* Flatten RMT symbols into signed-us durations following the Flipper RAW
 * convention: +value = carrier/mark (GDO0 high in OOK async RX), -value =
 * space (GDO0 low). The list is NORMALIZED so signs strictly alternate:
 *   - leading spaces (low before the first mark) are dropped (first is +),
 *   - the trailing space (the long low that ended the burst) is dropped
 *     (last is +),
 *   - two consecutive same-level fragments (e.g. a sub-min glitch merged away
 *     by hardware, or a field split) are SUMMED into one value rather than
 *     written as two same-sign values — preserving timing without breaking the
 *     alternating-sign invariant Flipper requires.
 * No value is ever zero. Returns the element count. */
static size_t symbols_to_durations(const rmt_symbol_word_t *sym, size_t num,
                                   int32_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < num; i++) {
        for (int half = 0; half < 2; half++) {
            uint16_t dur = half == 0 ? sym[i].duration0 : sym[i].duration1;
            uint16_t lvl = half == 0 ? sym[i].level0 : sym[i].level1;
            if (dur == 0) continue;
            int32_t v = lvl ? (int32_t)dur : -(int32_t)dur;
            if (n == 0) {
                if (v < 0) continue;            /* drop leading space(s) */
                if (dur < SUBGHZ_RX_DESPECKLE_US) continue; /* drop leading runt */
                out[n++] = v;
            } else if ((out[n - 1] < 0) == (v < 0)) {
                int64_t merged = (int64_t)out[n - 1] + v;   /* same level: merge */
                if (merged > INT32_MAX) merged = INT32_MAX;
                if (merged < INT32_MIN) merged = INT32_MIN;
                out[n - 1] = (int32_t)merged;
            } else if (dur < SUBGHZ_RX_DESPECKLE_US) {
                /* Short OPPOSITE-level spike: a demod glitch, not a real edge.
                 * Absorb its duration into the current level and stay on it, so
                 * the next (same-level) fragment merges in above. This removes
                 * the spike without breaking the strictly-alternating invariant. */
                int32_t mag = (int32_t)dur;
                int64_t merged = (int64_t)out[n - 1] + (out[n - 1] < 0 ? -mag : mag);
                if (merged > INT32_MAX) merged = INT32_MAX;
                if (merged < INT32_MIN) merged = INT32_MIN;
                out[n - 1] = (int32_t)merged;
            } else {
                if (n >= cap) break;
                out[n++] = v;
            }
        }
        if (n >= cap) break;
    }
    while (n > 0 && out[n - 1] < 0) n--;   /* end on a mark */
    return n;
}

/* Deliver one candidate frame: trim any residual leading/trailing space (the
 * splitter cuts at gaps, so this is defensive), drop noise fragments, time the
 * callback. `truncated` is true only when this frame may be incomplete. */
static void emit_one(const int32_t *d, size_t n, bool truncated)
{
    while (n > 0 && d[0] < 0) { d++; n--; }        /* lead on a mark */
    while (n > 0 && d[n - 1] < 0) n--;             /* end on a mark  */
    if (n < SUBGHZ_RX_MIN_EDGES || !s_cb) return;
    int64_t t0 = esp_timer_get_time();
    s_cb(d, n, truncated, s_ctx);
    uint32_t lat = (uint32_t)(esp_timer_get_time() - t0);
    if (lat > s_stats.max_cb_latency_us) s_stats.max_cb_latency_us = lat;
    s_stats.bursts++;
}

/* Software frame-end: split one normalized reception into frames at every low
 * of at least SUBGHZ_FRAME_GAP_US (the gap itself is a separator, delivered to
 * neither neighbour). A gap-closed frame is complete (truncated = false); only
 * the trailing remainder inherits the reception's truncated flag, since a full
 * RMT buffer may have cut it mid-frame. For the existing long-gap protocols the
 * reception holds a single frame with no interior gap >= the threshold, so this
 * makes exactly one callback — identical to the pre-Part-C behaviour. */
static void emit_frames(const int32_t *d, size_t n, bool truncated)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i] <= -(int32_t)SUBGHZ_FRAME_GAP_US) {   /* a low >= frame gap */
            emit_one(d + start, i - start, false);
            start = i + 1;
            s_stats.frame_splits++;
        }
    }
    if (start < n) emit_one(d + start, n - start, truncated);
}

static void rx_process_event(const subghz_rx_event_t *event)
{
    if (event->buffer_index >= SUBGHZ_RX_BUFFER_COUNT) {
        io_log("subghz: rx unknown buffer (%u)\r\n",
               (unsigned)event->buffer_index);
        s_stop_result = ESP_ERR_INVALID_STATE;
        s_rx_run = false;
        return;
    }
    if (event->num_symbols > SUBGHZ_RX_BUF_SYMBOLS) {
        s_stop_result = ESP_ERR_INVALID_SIZE;
        s_rx_run = false;
        return;
    }

    s_stats.received_symbols += (uint32_t)event->num_symbols;
    /* A full user buffer means the hardware may have dropped overflow symbols:
     * the burst is potentially truncated (lost edges). Track it so loss is
     * measurable rather than guessed, and flag it to the callback. */
    bool truncated = (event->num_symbols == SUBGHZ_RX_BUF_SYMBOLS);
    if (truncated) s_stats.truncated++;

    if (event->num_symbols == 0 || !s_cb) return;

    size_t dn = symbols_to_durations(s_rx_symbols[event->buffer_index],
                                     event->num_symbols, s_durations,
                                     SUBGHZ_RAW_MAX_TIMINGS);
    /* Split the reception into frames at inter-frame gaps, then deliver each
     * (noise fragments below MIN_EDGES are dropped inside emit_one). */
    emit_frames(s_durations, dn, truncated);
}

static void rx_task(void *arg)
{
    (void)arg;
    while (s_rx_run) {
        subghz_rx_event_t event;
        if (xQueueReceive(s_rx_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_event_overflow) {
                s_stats.buffer_overflow++;
                io_log("subghz: rx event overflow\r\n");
                s_event_overflow = false;
            }
            /* Re-arm the OTHER buffer immediately (ping-pong) before processing
             * so a back-to-back burst is not missed. */
            if (event.buffer_index < SUBGHZ_RX_BUFFER_COUNT && s_rx_run) {
                uint8_t next = event.buffer_index ^ 1U;
                esp_err_t err = rmt_receive(
                    s_rx_chan, s_rx_symbols[next],
                    SUBGHZ_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t), &s_rcfg);
                if (err != ESP_OK) {
                    io_log("subghz: rx re-arm failed (%d)\r\n", (int)err);
                    s_stop_result = err;
                    s_rx_run = false;
                } else {
                    s_stats.rmt_restarts++;
                }
            }
            rx_process_event(&event);
        }
    }

    /* Tear everything down HERE, tied to the task actually exiting, so a slow
     * exit that makes subghz_capture_stop() time out can never leave the RMT
     * channel allocated (which would brick every future start) or leak the
     * buffers. This runs after rmt is disabled, so the ISR/callback is quiet. */
    esp_err_t err = rmt_disable(s_rx_chan);
    if (err != ESP_OK) s_stop_result = err;
    err = rmt_del_channel(s_rx_chan);
    if (err != ESP_OK) s_stop_result = err;
    s_rx_chan = NULL;
    free_buffers();
    s_cb = NULL;
    s_ctx = NULL;
    s_rx_task = NULL;   /* cleared LAST: signals stop() teardown is complete */
    vTaskDelete(NULL);
}

esp_err_t subghz_capture_start(subghz_capture_cb_t cb, void *ctx)
{
    if (s_rx_run || s_rx_task || s_rx_chan) return ESP_ERR_INVALID_STATE;

    /* Fresh per-session counters; `sessions` stays cumulative. */
    uint32_t prev_sessions = s_stats.sessions;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.sessions = prev_sessions + 1;

    for (size_t i = 0; i < SUBGHZ_RX_BUFFER_COUNT; i++) {
        s_rx_symbols[i] = heap_caps_calloc(
            SUBGHZ_RX_BUF_SYMBOLS, sizeof(rmt_symbol_word_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_durations = heap_caps_calloc(SUBGHZ_RAW_MAX_TIMINGS, sizeof(int32_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_symbols[0] || !s_rx_symbols[1] || !s_durations) {
        io_log("subghz: rx buffer alloc failed\r\n");
        free_buffers();
        return ESP_ERR_NO_MEM;
    }

    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(SUBGHZ_RX_BUFFER_COUNT, sizeof(subghz_rx_event_t));
        if (!s_rx_queue) { free_buffers(); return ESP_ERR_NO_MEM; }
    }
    xQueueReset(s_rx_queue);

    rmt_rx_channel_config_t rxc = {
        .gpio_num = SUBGHZ_GDO0_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SUBGHZ_RX_RESOLUTION_HZ,
        .mem_block_symbols = SUBGHZ_RX_MEM_SYMBOLS,
        .flags.invert_in = false,   /* GDO0 async data is active-high */
        .flags.with_dma = false,
    };
    esp_err_t err = rmt_new_rx_channel(&rxc, &s_rx_chan);
    if (err != ESP_OK) {
        io_log("subghz: rx channel failed (%d) - none free?\r\n", (int)err);
        free_buffers();
        return err;
    }

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    err = rmt_rx_register_event_callbacks(s_rx_chan, &cbs, NULL);
    if (err != ESP_OK) { goto fail_chan; }

    err = rmt_enable(s_rx_chan);
    if (err != ESP_OK) { goto fail_chan; }

    s_cb = cb;
    s_ctx = ctx;
    s_stop_result = ESP_OK;
    s_event_overflow = false;
    s_rx_run = true;

    err = rmt_receive(s_rx_chan, s_rx_symbols[0],
                      SUBGHZ_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t), &s_rcfg);
    if (err != ESP_OK) {
        s_rx_run = false;
        rmt_disable(s_rx_chan);
        goto fail_chan;
    }

    if (xTaskCreate(rx_task, "sg_rx", 4096, NULL, 10, &s_rx_task) != pdPASS) {
        s_rx_run = false;
        rmt_disable(s_rx_chan);
        goto fail_chan;
    }
    return ESP_OK;

fail_chan:
    rmt_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    free_buffers();
    return err != ESP_OK ? err : ESP_ERR_NO_MEM;
}

esp_err_t subghz_capture_stop(void)
{
    if (!s_rx_run && !s_rx_task && !s_rx_chan) return ESP_OK;
    s_rx_run = false;
    /* Wait for the rx_task to exit; it frees the buffers and deletes the channel
     * itself (see rx_task end), so even if this times out nothing is leaked —
     * the task finishes the teardown on its own and a later start succeeds once
     * it has. */
    for (int i = 0; i < 150 && s_rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rx_task != NULL) {
        io_log("subghz: rx stop timed out (task self-cleans)\r\n");
        return ESP_ERR_TIMEOUT;
    }
    return s_stop_result;
}

void subghz_capture_get_stats(subghz_capture_stats_t *out)
{
    /* Snapshot of the diagnostic counters (updated from the rx task); values
     * are monotonic per session, so a momentary read is close enough for
     * diagnostics. */
    if (out) *out = s_stats;
}
