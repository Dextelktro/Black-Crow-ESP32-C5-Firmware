/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* ============================================================
 * IR front-end on the RMT peripheral.
 *
 *   TX: modulated carrier out of `tx_gpio` (default 38 kHz).
 *   RX: demodulated input on `rx_gpio` from a TSOP-style
 *       receiver (active-low output — invert_in is enabled so
 *       a carrier burst reads as a logic mark).
 *
 * Everything is expressed as microsecond timing lists, mark
 * first (see ir_proto.h). The RMT runs at 1 MHz (1 tick = 1 us).
 * ============================================================ */

/* Flipper caps raw signals at 1024 timings; match that. */
#define IR_RAW_MAX_TIMINGS 1024

typedef void (*ir_rx_cb_t)(const uint16_t *durations, size_t count, void *ctx);

esp_err_t ir_rmt_init(int tx_gpio, int rx_gpio);
bool      ir_rmt_ready(void);

/* Transmit a raw timing list once. Blocks until the frame is on the air.
 * carrier_hz 0 => 38000, duty 0 => 0.33. Durations are microseconds,
 * alternating mark/space starting with a mark; gaps longer than one RMT
 * symbol are split automatically. */
esp_err_t ir_rmt_send(const uint16_t *durations, size_t count,
                      uint32_t carrier_hz, float duty);

/* Same, for 32-bit timing lists (built-in raw library signals carry
 * inter-frame gaps that exceed 16 bits). */
esp_err_t ir_rmt_send_raw32(const uint32_t *durations, size_t count,
                            uint32_t carrier_hz, float duty);

/* Transmit a frame `repeats` times (>=1) in one RMT transaction, inserting
 * `gap_us` of silence between consecutive frames (never after the last). Used
 * by the universal sweep to send a short, controlled repeat (like Flipper's
 * per-protocol minimum) instead of replaying captured inter-frame dead air.
 * repeats<=1 / gap_us==0 behave exactly like the single-shot variants. */
esp_err_t ir_rmt_send_repeat(const uint16_t *durations, size_t count,
                             uint32_t carrier_hz, float duty,
                             int repeats, uint32_t gap_us);
esp_err_t ir_rmt_send_raw32_repeat(const uint32_t *durations, size_t count,
                                   uint32_t carrier_hz, float duty,
                                   int repeats, uint32_t gap_us);

/* Start/stop continuous reception. Each decoded burst is handed to `cb`
 * from an internal task (NOT an ISR — safe to log / allocate). */
esp_err_t ir_rmt_rx_start(ir_rx_cb_t cb, void *ctx);
esp_err_t ir_rmt_rx_stop(void);
bool      ir_rmt_rx_active(void);
