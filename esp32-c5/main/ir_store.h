#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "ir_proto.h"
#include "ir_rmt.h"

/* ============================================================
 * Persistent storage for captured IR signals on a LittleFS
 * partition ("storage", mounted at /ir). Each saved signal is
 * one Flipper-compatible .ir file holding a single RAW button,
 * plus comment lines carrying our decoded metadata.
 * ============================================================ */

#define IR_NAME_MAX 48

typedef void (*ir_store_list_cb)(const char *name, size_t size_bytes,
                                 void *ctx);

esp_err_t ir_store_init(void);
bool      ir_store_ready(void);

esp_err_t ir_store_save(const char *name,
                        const uint16_t *durations, size_t count,
                        uint32_t carrier_hz, float duty,
                        const ir_message_t *decoded /* may be NULL */);

esp_err_t ir_store_list(ir_store_list_cb cb, void *ctx);
int       ir_store_count(void);

esp_err_t ir_store_load(const char *name,
                        uint16_t *out, size_t out_cap, size_t *out_count,
                        uint32_t *carrier_hz, float *duty,
                        ir_message_t *decoded /* may be NULL */);

esp_err_t ir_store_delete(const char *name);
esp_err_t ir_store_usage(size_t *total, size_t *used);
