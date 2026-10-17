#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ir_db.h"

/* ============================================================
 * Infrared feature module — universal remote, receiver with a
 * live spectrum, and capture/emulate backed by LittleFS.
 *
 * Pins: TX = GPIO4, RX = GPIO5 (RMT peripheral; independent of
 * the WiFi / BLE / nRF24 radios, so IR never contends for them).
 * ============================================================ */

#define IR_TX_GPIO 4
#define IR_RX_GPIO 5

esp_err_t ir_module_init(void);
bool      ir_module_ready(void);

/* Universal remote: sweep every stored code for (category, button). */
esp_err_t ir_universal_send(ir_cat_t cat, const char *button);
void      ir_universal_stop(void);
bool      ir_universal_running(void);

/* Receiver (capture + decode) and its live spectrum envelope. */
esp_err_t ir_rx_start(void);
esp_err_t ir_rx_stop(void);
esp_err_t ir_capture_history_clear(void);
bool      ir_rx_running(void);
void      ir_spectrum_set(bool on);
bool      ir_spectrum_enabled(void);

/* Persist the latest capture (id 0) or a retained receiver capture by id. */
esp_err_t ir_save_last(const char *name);
esp_err_t ir_save_capture(const char *name, uint32_t capture_id);
bool      ir_have_last(void);

/* Saved library. */
esp_err_t ir_saved_list(void);
esp_err_t ir_saved_info(const char *name);
esp_err_t ir_saved_delete(const char *name);

/* Emulate (transmit) a saved signal, repeating until stopped
 * (count 0 = loop indefinitely). */
esp_err_t ir_emulate_start(const char *name, int count);
void      ir_emulate_stop(void);
bool      ir_emulate_running(void);

void      ir_status(void);
