#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ---- lifecycle (powered down / sleeping while idle) ---- */
esp_err_t cc1101_init(void);
esp_err_t cc1101_sleep(void);
esp_err_t cc1101_wake(void);
esp_err_t cc1101_deinit(void);
bool cc1101_is_present(void);
bool cc1101_is_sleeping(void);

/* ---- register / strobe primitives ---- */
esp_err_t cc1101_strobe(uint8_t strobe, uint8_t *status_out);
esp_err_t cc1101_write_reg(uint8_t addr, uint8_t value);
esp_err_t cc1101_read_reg(uint8_t addr, uint8_t *value);
esp_err_t cc1101_read_status(uint8_t addr, uint8_t *value);
esp_err_t cc1101_write_burst(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t cc1101_write_patable(const uint8_t pa[8]);

/* ---- higher-level helpers ---- */
esp_err_t cc1101_reset(void);
esp_err_t cc1101_idle(void);
esp_err_t cc1101_flush_rx(void);
esp_err_t cc1101_flush_tx(void);
esp_err_t cc1101_set_frequency(uint32_t hz);
uint32_t  cc1101_get_frequency(void);
int       cc1101_read_rssi_dbm(void);
int       cc1101_rssi_raw_to_dbm(uint8_t raw);
esp_err_t cc1101_read_marcstate(uint8_t *state);
