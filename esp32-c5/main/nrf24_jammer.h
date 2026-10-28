/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "nrf24_driver.h"

typedef enum {
    JAM_STRAT_CW = 0,
    JAM_STRAT_FLOOD,
    JAM_STRAT_REACTIVE,
} jam_strategy_t;

typedef enum {
    JAM_TARGET_FULL = 0,
    JAM_TARGET_WIFI,
    JAM_TARGET_BLE_ADV,
    JAM_TARGET_BLE_DATA,
    JAM_TARGET_BT_CLASSIC,
    JAM_TARGET_ZIGBEE,
    JAM_TARGET_DRONE,
    JAM_TARGET_USB_DONGLE,
    JAM_TARGET_COUNT
} jam_target_t;

typedef struct {
    jam_target_t target;
    jam_strategy_t strategy;
    nrf24_rate_t rate;
    nrf24_pa_t pa;
    uint32_t dwell_us;
    uint8_t burst;
    bool random_hop;
} jam_config_t;

esp_err_t nrf24_jammer_init(void);
esp_err_t nrf24_jammer_start_manual(const jam_config_t *cfg, uint32_t duration_s);
esp_err_t nrf24_jammer_start_auto(jam_target_t target, uint32_t duration_s);
esp_err_t nrf24_jammer_start_waterfall(uint32_t duration_s);
esp_err_t nrf24_jammer_stop(void);
bool      nrf24_jammer_is_running(void);

void nrf24_jammer_default_config(jam_target_t target, jam_config_t *out);
const char *nrf24_jammer_target_name(jam_target_t t);
const char *nrf24_jammer_strategy_name(jam_strategy_t s);
