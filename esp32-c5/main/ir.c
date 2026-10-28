/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ir.h"

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "io.h"
#include "ir_rmt.h"
#include "ir_proto.h"
#include "ir_db.h"
#include "ir_store.h"

/* Unleashed schedules one universal candidate per 100 ms UI tick. Pace by
 * start-to-start interval: short frames wait for the next slot; long frames
 * naturally occupy the slot without an extra delay. */
#define IR_UNIV_PERIOD_US 100000
#define IR_EMIT_GAP_MS   130
/* Fallback gap for parsed protocols without a protocol-specific repeat period. */
#define IR_UNIV_REPEAT_GAP_US 20000
#define IR_SPEC_BINS     48
#define IR_CAPTURE_HISTORY_MAX 40

typedef struct {
    uint32_t id;
    uint16_t durations[IR_RAW_MAX_TIMINGS];
    size_t count;
    ir_message_t message;
} ir_capture_t;

static bool s_ready = false;

/* ── Last captured signal (for `ir save`) ───────────────────── */
static SemaphoreHandle_t s_last_mux = NULL;
static SemaphoreHandle_t s_scratch_mux = NULL;
static uint16_t *s_last;
static size_t    s_last_count = 0;
static ir_message_t s_last_msg;
static uint32_t  s_last_carrier = 38000;
static float     s_last_duty = 0.33f;
static bool      s_have_last = false;
static ir_capture_t *s_capture_history;
static uint32_t s_capture_next_id;

/* ── Receiver / spectrum ────────────────────────────────────── */
static volatile bool s_rx_on = false;
static volatile bool s_spectrum_on = false;

/* ── TX worker (universal OR emulate — mutually exclusive) ───── */
static TaskHandle_t  s_tx_task = NULL;
static volatile bool s_tx_busy = false;
static volatile bool s_tx_stop = false;
static volatile bool s_tx_is_emulate = false;
static bool s_tx_finishing = false;
static portMUX_TYPE s_tx_state_mux = portMUX_INITIALIZER_UNLOCKED;

/* Universal job params. */
static ir_cat_t s_univ_cat;
static char     s_univ_btn[24];
/* Flipper toggles these on each parsed message. */
static bool s_rc5_toggle;
static bool s_rc6_toggle;

/* Emulate job params. */
static char s_emit_name[IR_NAME_MAX + 1];
static int  s_emit_count;

/* Scratch timing buffer for the saved-library reads (used only from the
 * single-threaded CLI command task — list and info never overlap). */
static uint16_t *s_scratch;

bool ir_module_ready(void) { return s_ready; }
bool ir_rx_running(void)   { return s_rx_on; }
bool ir_spectrum_enabled(void) { return s_spectrum_on; }
bool ir_universal_running(void) { return s_tx_busy && !s_tx_is_emulate; }
bool ir_emulate_running(void)   { return s_tx_busy && s_tx_is_emulate; }
bool ir_have_last(void)    { return s_have_last; }

/* ------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------ */

/* Reconstruct the protocol data bytes (for display). Returns count. */
static int decoded_bytes(const ir_message_t *m, uint8_t *out)
{
    switch (m->proto) {
    case IR_PROTO_NEC:
        out[0] = m->address & 0xFF; out[1] = ~out[0];
        out[2] = m->command & 0xFF; out[3] = ~out[2];
        return 4;
    case IR_PROTO_NECEXT:
    case IR_PROTO_PIONEER:
        out[0] = m->address & 0xFF; out[1] = (m->address >> 8) & 0xFF;
        out[2] = m->command & 0xFF; out[3] = ~out[2];
        return 4;
    case IR_PROTO_SAMSUNG32:
        out[0] = m->address & 0xFF; out[1] = out[0];
        out[2] = m->command & 0xFF; out[3] = ~out[2];
        return 4;
    case IR_PROTO_SONY: {
        uint32_t v = (m->command & 0x7F) | (m->address << 7);
        out[0] = v & 0xFF; out[1] = (v >> 8) & 0xFF; out[2] = (v >> 16) & 0xFF;
        return (m->bits > 15) ? 3 : 2;
    }
    case IR_PROTO_RCA:
        out[0] = (m->address & 0x0F); out[1] = m->command & 0xFF;
        return 2;
    default:
        return 0;
    }
}

static void emit_sig(const ir_message_t *m, size_t n, uint32_t id)
{
    uint8_t b[8];
    int nb = decoded_bytes(m, b);
    char bytes[32] = "-";
    if (nb > 0) {
        int off = 0;
        for (int i = 0; i < nb && off < (int)sizeof(bytes) - 3; i++) {
            off += snprintf(bytes + off, sizeof(bytes) - off,
                            (i == 0) ? "%02X" : ".%02X", b[i]);
        }
    }
    io_log("ir_rx: SIG proto=%s addr=0x%lX cmd=0x%lX bits=%u n=%u bytes=%s id=%" PRIu32 "\r\n",
           ir_proto_name(m->proto), (unsigned long)m->address,
           (unsigned long)m->command, m->bits, (unsigned)n, bytes, id);
}

/* Stream the raw timings, chunked to stay under io_log's 256-byte line. */
static void emit_raw(const uint16_t *d, size_t n)
{
    const size_t per = 24;
    for (size_t off = 0; off < n; off += per) {
        char line[256];
        int p = snprintf(line, sizeof(line), "ir_rx: RAW n=%u off=%u d=",
                         (unsigned)n, (unsigned)off);
        for (size_t i = off; i < n && i < off + per; i++) {
            p += snprintf(line + p, sizeof(line) - p, "%u ", (unsigned)d[i]);
            if (p >= (int)sizeof(line) - 8) break;
        }
        io_log("%s\r\n", line);
    }
}

/* Downsample a burst into IR_SPEC_BINS mark-coverage bins (0..100). */
static void emit_wave(const uint16_t *d, size_t n)
{
    uint32_t total = 0;
    for (size_t i = 0; i < n; i++) total += d[i];
    if (total == 0) return;

    double bin_w = (double)total / IR_SPEC_BINS;
    double mark_in_bin[IR_SPEC_BINS] = {0};

    double t = 0;
    for (size_t i = 0; i < n; i++) {
        double seg_start = t;
        double seg_end = t + d[i];
        t = seg_end;
        bool is_mark = ((i & 1) == 0);   /* even index = mark */
        if (!is_mark) continue;
        int b0 = (int)(seg_start / bin_w);
        int b1 = (int)(seg_end / bin_w);
        if (b1 >= IR_SPEC_BINS) b1 = IR_SPEC_BINS - 1;
        for (int b = b0; b <= b1 && b < IR_SPEC_BINS; b++) {
            double lo = b * bin_w, hi = (b + 1) * bin_w;
            double ov_lo = seg_start > lo ? seg_start : lo;
            double ov_hi = seg_end < hi ? seg_end : hi;
            if (ov_hi > ov_lo) mark_in_bin[b] += (ov_hi - ov_lo);
        }
    }

    char line[256];
    int p = snprintf(line, sizeof(line), "ir_spec: WAVE nb=%d total=%u d=",
                     IR_SPEC_BINS, (unsigned)total);
    for (int b = 0; b < IR_SPEC_BINS; b++) {
        int v = (int)(mark_in_bin[b] / bin_w * 100.0 + 0.5);
        if (v > 100) v = 100;
        p += snprintf(line + p, sizeof(line) - p, "%d ", v);
        if (p >= (int)sizeof(line) - 6) break;
    }
    io_log("%s\r\n", line);
}

/* ------------------------------------------------------------------
 * Receiver callback (runs in the ir_rx task, not an ISR)
 * ------------------------------------------------------------------ */
static void rx_burst(const uint16_t *durations, size_t count, void *ctx)
{
    (void)ctx;
    ir_message_t msg;
    uint32_t capture_id = 0;
    ir_decode(durations, count, &msg);  /* proto UNKNOWN if not matched */
    if (msg.proto == IR_PROTO_UNKNOWN) msg.proto = IR_PROTO_RAW;

    /* Latch as the last capture (always store raw timings). */
    if (s_last_mux && xSemaphoreTake(s_last_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t c = count > IR_RAW_MAX_TIMINGS ? IR_RAW_MAX_TIMINGS : count;
        memcpy(s_last, durations, c * sizeof(uint16_t));
        s_last_count = c;
        s_last_msg = msg;
        s_last_carrier = 38000;  /* raw capture replays at the common carrier */
        s_last_duty = 0.33f;
        s_have_last = true;
        capture_id = ++s_capture_next_id;
        if (capture_id == 0) capture_id = ++s_capture_next_id;
        if (s_capture_history) {
            ir_capture_t *capture =
                &s_capture_history[(capture_id - 1U) % IR_CAPTURE_HISTORY_MAX];
            capture->id = capture_id;
            capture->count = c;
            capture->message = msg;
            memcpy(capture->durations, durations, c * sizeof(uint16_t));
        } else {
            capture_id = 0;
        }
        xSemaphoreGive(s_last_mux);
    }

    emit_sig(&msg, count, capture_id);
    emit_raw(durations, count);
    if (s_spectrum_on) emit_wave(durations, count);
}

/* ------------------------------------------------------------------
 * Receiver / spectrum control
 * ------------------------------------------------------------------ */
esp_err_t ir_rx_start(void)
{
    if (!s_ready) { io_log("ir: not ready\r\n"); return ESP_ERR_INVALID_STATE; }
    if (s_tx_busy) { io_log("ir: busy (tx)\r\n"); return ESP_ERR_INVALID_STATE; }
    if (s_rx_on && ir_rmt_rx_active()) return ESP_OK;
    if (s_rx_on) {
        s_rx_on = false;
        io_log("ir: rx worker stopped; attempting recovery\r\n");
    }
    esp_err_t err = ir_rmt_rx_start(rx_burst, NULL);
    if (err != ESP_OK) { io_log("ir: rx start failed\r\n"); return err; }
    s_rx_on = true;
    io_log("ir_rx: START\r\n");
    return ESP_OK;
}

esp_err_t ir_rx_stop(void)
{
    if (!s_rx_on) return ESP_OK;
    esp_err_t err = ir_rmt_rx_stop();
    if (err != ESP_OK) {
        io_log("ir: rx stop failed (%d)\r\n", (int)err);
        return err;
    }
    s_rx_on = false;
    io_log("ir_rx: STOPPED\r\n");
    return ESP_OK;
}

void ir_spectrum_set(bool on)
{
    s_spectrum_on = on;
    io_log(on ? "ir_spec: ON\r\n" : "ir_spec: OFF\r\n");
}

/* ------------------------------------------------------------------
 * TX worker (universal sweep / emulate loop)
 * ------------------------------------------------------------------ */
static void tx_task(void *arg)
{
    (void)arg;

    if (s_tx_is_emulate) {
        /* Load the saved raw signal once, then repeat. */
        static uint16_t buf[IR_RAW_MAX_TIMINGS];
        size_t n = 0;
        uint32_t carrier = 38000;
        float duty = 0.33f;
        esp_err_t err = ir_store_load(s_emit_name, buf, IR_RAW_MAX_TIMINGS,
                                      &n, &carrier, &duty, NULL);
        if (err != ESP_OK || n == 0) {
            taskENTER_CRITICAL(&s_tx_state_mux);
            s_tx_finishing = true;
            bool stopped = s_tx_stop;
            taskEXIT_CRITICAL(&s_tx_state_mux);
            io_log("ir: EMIT_FAIL name=%s err=%d\r\n", s_emit_name,
                   (int)(err == ESP_OK ? ESP_ERR_INVALID_SIZE : err));
            io_log("ir: EMIT_DONE name=%s sent=0 failed=1 stopped=%d\r\n",
                   s_emit_name, (int)stopped);
        } else {
            io_log("ir: EMIT name=%s count=%d n=%u\r\n",
                   s_emit_name, s_emit_count, (unsigned)n);
            int sent = 0;
            int failed = 0;
            while (!s_tx_stop) {
                err = ir_rmt_send(buf, n, carrier, duty);
                if (err != ESP_OK) {
                    failed++;
                    io_log("ir: EMIT_FAIL name=%s err=%d\r\n",
                           s_emit_name, (int)err);
                    break;
                }
                sent++;
                if (s_emit_count > 0 && sent >= s_emit_count) break;
                for (int i = 0; i < IR_EMIT_GAP_MS / 10 && !s_tx_stop; i++) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            taskENTER_CRITICAL(&s_tx_state_mux);
            s_tx_finishing = true;
            bool stopped = s_tx_stop;
            taskEXIT_CRITICAL(&s_tx_state_mux);
            io_log("ir: EMIT_DONE name=%s sent=%d failed=%d stopped=%d\r\n",
                   s_emit_name, sent, failed, (int)stopped);
        }
    } else {
        int total = ir_db_count(s_univ_cat, s_univ_btn);
        io_log("ir: UNIV cat=%s btn=%s total=%d\r\n",
               ir_cat_name(s_univ_cat), s_univ_btn, total);
        int sent = 0;
        int failed = 0;
        size_t db_cursor = 0;
        for (int i = 0; i < total && !s_tx_stop; i++) {
            const ir_db_entry_t *e = ir_db_next(s_univ_cat, s_univ_btn, &db_cursor);
            if (!e) {
                failed += total - i;
                io_log("ir: DB cursor ended early at %d/%d; counted remaining entries as failed\r\n",
                       i, total);
                break;
            }
            int64_t tx_start_us = esp_timer_get_time();
            esp_err_t err;
            size_t raw_sent_count = 0;
            uint64_t raw_total_us = 0;
            if (e->proto == IR_PROTO_RAW && e->raw && e->raw_len) {
                /* Preserve the source capture. Long spaces can separate required
                 * repeats or parts of a command; Unleashed sends the whole RAW. */
                raw_sent_count = e->raw_len;
                for (size_t j = 0; j < e->raw_len; j++) {
                    raw_total_us += e->raw[j];
                }
                err = ir_rmt_send_raw32(
                    e->raw, e->raw_len,
                    e->carrier ? e->carrier : 38000,
                    e->duty > 0.0f ? e->duty : 0.33f);
            } else {
                ir_message_t m = {
                    .proto = e->proto, .address = e->address,
                    .command = e->command, .bits = e->bits,
                };
                if (e->proto == IR_PROTO_RC5) {
                    m.toggle = s_rc5_toggle;
                    s_rc5_toggle = !s_rc5_toggle;
                } else if (e->proto == IR_PROTO_RC6) {
                    m.toggle = s_rc6_toggle;
                    s_rc6_toggle = !s_rc6_toggle;
                }
                uint16_t buf[IR_ENCODE_MAX_TIMINGS];
                uint32_t carrier = 38000;
                float duty = 0.33f;
                size_t n = ir_encode(&m, buf, IR_ENCODE_MAX_TIMINGS,
                                     &carrier, &duty);
                uint32_t repeat_gap_us = IR_UNIV_REPEAT_GAP_US;
                if (e->proto == IR_PROTO_PIONEER) {
                    repeat_gap_us = 26000;
                } else if (e->proto == IR_PROTO_SONY && n > 0) {
                    uint64_t frame_us = 0;
                    for (size_t j = 0; j < n; j++) frame_us += buf[j];
                    repeat_gap_us = frame_us < 45000
                                        ? (uint32_t)(45000 - frame_us)
                                        : 0;
                }
                err = n == 0
                    ? ESP_ERR_NOT_SUPPORTED
                    : ir_rmt_send_repeat(buf, n, carrier, duty,
                                         ir_proto_min_repeats(e->proto),
                                         repeat_gap_us);
            }
            int64_t tx_time_us = esp_timer_get_time();
            if (err == ESP_OK) {
                sent++;
                io_log("ir: TX i=%d/%d t_us=%" PRId64 " brand=\"%s\" proto=%s raw_n=%u raw_us=%" PRIu64 "\r\n",
                       i + 1, total, tx_time_us, e->brand ? e->brand : "?",
                       ir_proto_name(e->proto), (unsigned)raw_sent_count,
                       raw_total_us);
            } else {
                failed++;
                io_log("ir: TX_FAIL i=%d/%d t_us=%" PRId64 " brand=\"%s\" err=%d raw_n=%u raw_us=%" PRIu64 "\r\n",
                       i + 1, total, tx_time_us, e->brand ? e->brand : "?",
                       (int)err, (unsigned)raw_sent_count, raw_total_us);
            }
            /* Match Flipper's 100 ms candidate cadence without adding a full
             * period after long frames. */
            if (!s_tx_stop) {
                uint64_t elapsed_us = (uint64_t)(tx_time_us - tx_start_us);
                if (elapsed_us < IR_UNIV_PERIOD_US) {
                    uint32_t wait_ms = (uint32_t)(
                        (IR_UNIV_PERIOD_US - elapsed_us + 999ULL) / 1000ULL);
                    if (wait_ms > 0) {
                        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
                        if (wait_ticks == 0) wait_ticks = 1;
                        vTaskDelay(wait_ticks);
                    }
                }
            }
        }
        taskENTER_CRITICAL(&s_tx_state_mux);
        s_tx_finishing = true;
        bool stopped = s_tx_stop;
        taskEXIT_CRITICAL(&s_tx_state_mux);
        io_log("ir: UNIV_DONE cat=%s btn=%s sent=%d failed=%d stopped=%d\r\n",
               ir_cat_name(s_univ_cat), s_univ_btn, sent, failed,
               (int)stopped);
    }

    taskENTER_CRITICAL(&s_tx_state_mux);
    s_tx_busy = false;
    s_tx_stop = false;
    s_tx_finishing = false;
    s_tx_task = NULL;
    taskEXIT_CRITICAL(&s_tx_state_mux);
    vTaskDelete(NULL);
}

static esp_err_t tx_start(bool emulate, ir_cat_t cat, const char *button,
                          const char *name, int count)
{
    if (!s_ready) { io_log("ir: not ready\r\n"); return ESP_ERR_INVALID_STATE; }
    taskENTER_CRITICAL(&s_tx_state_mux);
    if (s_tx_busy || s_tx_task) {
        taskEXIT_CRITICAL(&s_tx_state_mux);
        io_log("ir: busy\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_tx_is_emulate = emulate;
    s_tx_stop = false;
    s_tx_finishing = false;
    s_tx_busy = true;
    if (emulate) {
        strncpy(s_emit_name, name, sizeof(s_emit_name) - 1);
        s_emit_name[sizeof(s_emit_name) - 1] = '\0';
        s_emit_count = count;
    } else {
        s_univ_cat = cat;
        strncpy(s_univ_btn, button, sizeof(s_univ_btn) - 1);
        s_univ_btn[sizeof(s_univ_btn) - 1] = '\0';
    }
    taskEXIT_CRITICAL(&s_tx_state_mux);

    /* Receiver and transmitter share the IR front-end timing; stop RX. */
    if (s_rx_on) {
        esp_err_t err = ir_rx_stop();
        if (err != ESP_OK) {
            taskENTER_CRITICAL(&s_tx_state_mux);
            s_tx_busy = false;
            s_tx_stop = false;
            s_tx_task = NULL;
            taskEXIT_CRITICAL(&s_tx_state_mux);
            return err;
        }
    }

    if (xTaskCreate(tx_task, "ir_tx", 4096, NULL, 9, &s_tx_task) != pdPASS) {
        taskENTER_CRITICAL(&s_tx_state_mux);
        s_tx_busy = false;
        s_tx_task = NULL;
        taskEXIT_CRITICAL(&s_tx_state_mux);
        io_log("ir: tx task create failed\r\n");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ir_universal_send(ir_cat_t cat, const char *button)
{
    if (cat < IR_CAT_TV || cat >= IR_CAT_COUNT || !button) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ir_db_count(cat, button) == 0) {
        io_log("ir: UNIV cat=%s btn=%s total=0\r\n", ir_cat_name(cat), button);
        io_log("ir: no built-in codes — capture & save instead\r\n");
        return ESP_ERR_NOT_FOUND;
    }
    return tx_start(false, cat, button, NULL, 0);
}

void ir_universal_stop(void)
{
    int state = 0;
    taskENTER_CRITICAL(&s_tx_state_mux);
    if (s_tx_busy && !s_tx_is_emulate) {
        if (s_tx_stop || s_tx_finishing) {
            state = 1;
        } else {
            s_tx_stop = true;
            state = 2;
        }
    }
    taskEXIT_CRITICAL(&s_tx_state_mux);
    if (state == 2) {
        io_log("ir: univ stop requested\r\n");
    } else if (state == 1) {
        io_log("ir: univ stop pending\r\n");
    } else {
        io_log("ir: univ stop idle\r\n");
    }
}

esp_err_t ir_emulate_start(const char *name, int count)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    return tx_start(true, IR_CAT_TV, NULL, name, count);
}

void ir_emulate_stop(void)
{
    int state = 0;
    taskENTER_CRITICAL(&s_tx_state_mux);
    if (s_tx_busy && s_tx_is_emulate) {
        if (s_tx_stop || s_tx_finishing) {
            state = 1;
        } else {
            s_tx_stop = true;
            state = 2;
        }
    }
    taskEXIT_CRITICAL(&s_tx_state_mux);
    if (state == 2) {
        io_log("ir_emit: STOPPED\r\n");
    } else if (state == 1) {
        io_log("ir_emit: STOP_PENDING\r\n");
    } else {
        io_log("ir_emit: STOP_IDLE\r\n");
    }
}

/* ------------------------------------------------------------------
 * Save / saved library
 * ------------------------------------------------------------------ */
esp_err_t ir_save_capture(const char *name, uint32_t capture_id)
{
    if (!s_have_last || s_last_count == 0) {
        io_log("ir: NOCAP\r\n");
        return ESP_ERR_NOT_FOUND;
    }
    /* Decouple the flash write from active listening: briefly pause the RMT
     * RX hardware (app-visible state unchanged), write, then resume. Avoids
     * writing flash while the receiver is capturing. */
    bool was_rx = s_rx_on;
    if (was_rx) {
        esp_err_t stop_err = ir_rmt_rx_stop();
        if (stop_err != ESP_OK) {
            io_log("ir: cannot pause rx for save (%d)\r\n", (int)stop_err);
            return stop_err;
        }
        s_rx_on = false;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (s_last_mux && xSemaphoreTake(s_last_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (capture_id == 0) {
            err = ir_store_save(name, s_last, s_last_count, s_last_carrier,
                                s_last_duty, &s_last_msg);
        } else if (s_capture_history) {
            ir_capture_t *capture =
                &s_capture_history[(capture_id - 1U) % IR_CAPTURE_HISTORY_MAX];
            if (capture->id == capture_id && capture->count > 0) {
                err = ir_store_save(name, capture->durations, capture->count,
                                    38000, 0.33f, &capture->message);
            }
        } else if (capture_id == s_capture_next_id) {
            err = ir_store_save(name, s_last, s_last_count, s_last_carrier,
                                s_last_duty, &s_last_msg);
        }
        xSemaphoreGive(s_last_mux);
    } else if (s_last_mux) {
        err = ESP_ERR_TIMEOUT;
    } else {
        err = ir_store_save(name, s_last, s_last_count, s_last_carrier,
                            s_last_duty, &s_last_msg);
    }

    if (was_rx) {
        esp_err_t resume_err = ir_rmt_rx_start(rx_burst, NULL);
        if (resume_err == ESP_OK) {
            s_rx_on = true;
        } else {
            io_log("ir: rx resume after save failed (%d)\r\n", (int)resume_err);
            if (err == ESP_OK) err = resume_err;
        }
    }
    if (err == ESP_ERR_NOT_FOUND) {
        io_log("ir: capture expired id=%" PRIu32 "\r\n", capture_id);
        return err;
    }
    if (err == ESP_ERR_TIMEOUT) {
        io_log("ir: capture save timed out\r\n");
        return err;
    }
    if (err != ESP_OK) io_log("ir: save failed (%d)\r\n", err);
    return err;
}

esp_err_t ir_save_last(const char *name)
{
    return ir_save_capture(name, 0);
}

esp_err_t ir_capture_history_clear(void)
{
    if (s_rx_on) return ESP_ERR_INVALID_STATE;
    if (!s_last_mux ||
        xSemaphoreTake(s_last_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_capture_history) {
        memset(s_capture_history, 0,
               IR_CAPTURE_HISTORY_MAX * sizeof(*s_capture_history));
    }
    s_capture_next_id = 0;
    s_last_count = 0;
    s_have_last = false;
    xSemaphoreGive(s_last_mux);
    io_log("ir_rx: CLEARED\r\n");
    return ESP_OK;
}

static void list_cb(const char *name, size_t sz, void *ctx)
{
    (void)ctx;
    size_t n = 0;
    ir_message_t m;
    uint32_t carrier = 38000;
    float duty = 0.33f;
    if (ir_store_load(name, s_scratch, IR_RAW_MAX_TIMINGS, &n, &carrier, &duty, &m)
            == ESP_OK) {
        io_log("ir_saved: ENT name=%s size=%u proto=%s addr=0x%lX cmd=0x%lX n=%u\r\n",
               name, (unsigned)sz, ir_proto_name(m.proto),
               (unsigned long)m.address, (unsigned long)m.command, (unsigned)n);
    } else {
        io_log("ir_saved: ENT name=%s size=%u proto=? addr=0x0 cmd=0x0 n=0\r\n",
               name, (unsigned)sz);
    }
}

esp_err_t ir_saved_list(void)
{
    int c = ir_store_count();
    io_log("ir_saved: LS count=%d\r\n", c);
    /* Always emit the "LS end" terminator the app keys on, even on an early
     * exit, or the saved-list load never completes in the UI. */
    if (!s_scratch_mux || !s_scratch) {
        io_log("ir_saved: LS end\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_scratch_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
        io_log("ir_saved: LS end\r\n");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ir_store_list(list_cb, NULL);
    xSemaphoreGive(s_scratch_mux);
    io_log("ir_saved: LS end\r\n");
    return err;
}

esp_err_t ir_saved_info(const char *name)
{
    if (!s_scratch_mux || !s_scratch) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_scratch_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
        io_log("ir: info busy name=%s\r\n", name);
        return ESP_ERR_TIMEOUT;
    }
    size_t n = 0;
    ir_message_t m;
    uint32_t carrier = 38000;
    float duty = 0.33f;
    esp_err_t err = ir_store_load(name, s_scratch, IR_RAW_MAX_TIMINGS, &n,
                                  &carrier, &duty, &m);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scratch_mux);
        io_log("ir: info not found name=%s\r\n", name);
        return err;
    }
    io_log("ir_saved: INFO name=%s proto=%s addr=0x%lX cmd=0x%lX freq=%lu n=%u\r\n",
           name, ir_proto_name(m.proto), (unsigned long)m.address,
           (unsigned long)m.command, (unsigned long)carrier, (unsigned)n);
    emit_raw(s_scratch, n);
    xSemaphoreGive(s_scratch_mux);
    return ESP_OK;
}

esp_err_t ir_saved_delete(const char *name)
{
    return ir_store_delete(name);
}

void ir_status(void)
{
    size_t total = 0, used = 0;
    ir_store_usage(&total, &used);
    io_log("ir: status ready=%d rx=%d spec=%d tx=%d emulate=%d saved=%d "
           "fs=%u/%u have_last=%d\r\n",
           (int)s_ready, (int)s_rx_on, (int)s_spectrum_on,
           (int)ir_universal_running(), (int)ir_emulate_running(),
           ir_store_count(), (unsigned)used, (unsigned)total, (int)s_have_last);
}

/* ------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------ */
esp_err_t ir_module_init(void)
{
    if (s_ready) return ESP_OK;
    memset(&s_last_msg, 0, sizeof(s_last_msg));
    if (!s_last_mux) s_last_mux = xSemaphoreCreateMutex();
    if (!s_last_mux) return ESP_ERR_NO_MEM;
    if (!s_scratch_mux) s_scratch_mux = xSemaphoreCreateMutex();
    if (!s_scratch_mux) {
        vSemaphoreDelete(s_last_mux);
        s_last_mux = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Prefer PSRAM (keeps internal RAM free) but fall back to internal heap so
     * IR still comes up when PSRAM is exhausted — only a true double-failure
     * aborts init. */
    if (!s_last) {
        s_last = heap_caps_calloc(IR_RAW_MAX_TIMINGS, sizeof(*s_last),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_last) {
            s_last = heap_caps_calloc(IR_RAW_MAX_TIMINGS, sizeof(*s_last),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_scratch) {
        s_scratch = heap_caps_calloc(IR_RAW_MAX_TIMINGS, sizeof(*s_scratch),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scratch) {
            s_scratch = heap_caps_calloc(IR_RAW_MAX_TIMINGS, sizeof(*s_scratch),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_last || !s_scratch) {
        ESP_LOGE("ir", "timing buffer allocation failed (PSRAM + internal)");
        heap_caps_free(s_last);
        s_last = NULL;
        heap_caps_free(s_scratch);
        s_scratch = NULL;
        vSemaphoreDelete(s_scratch_mux);
        s_scratch_mux = NULL;
        vSemaphoreDelete(s_last_mux);
        s_last_mux = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (!s_capture_history) {
        s_capture_history = heap_caps_calloc(
            IR_CAPTURE_HISTORY_MAX, sizeof(*s_capture_history),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_capture_history) {
        ESP_LOGW("ir", "capture history allocation failed; only latest capture can be saved");
    }

    esp_err_t err = ir_rmt_init(IR_TX_GPIO, IR_RX_GPIO);
    if (err != ESP_OK) {
        heap_caps_free(s_capture_history);
        s_capture_history = NULL;
        heap_caps_free(s_scratch);
        s_scratch = NULL;
        heap_caps_free(s_last);
        s_last = NULL;
        vSemaphoreDelete(s_scratch_mux);
        s_scratch_mux = NULL;
        vSemaphoreDelete(s_last_mux);
        s_last_mux = NULL;
        io_log("ir: rmt init failed (%d)\r\n", err);
        return err;
    }
    /* Storage is best-effort: IR TX/RX still works without it. */
    if (ir_store_init() != ESP_OK) {
        io_log("ir: storage unavailable — save/emulate disabled\r\n");
    }
    s_ready = true;
    io_log("ir: module ready tx=%d rx=%d\r\n", IR_TX_GPIO, IR_RX_GPIO);
    return ESP_OK;
}
