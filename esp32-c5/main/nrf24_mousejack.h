/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define MOUSEJACK_MAX_TARGETS  16
#define MOUSEJACK_ADDR_LEN     5

typedef struct {
    uint8_t  address[MOUSEJACK_ADDR_LEN];
    uint8_t  channel;
    uint32_t hits;
    bool     is_logitech;
    bool     is_microsoft;
} mousejack_target_t;

esp_err_t nrf24_mousejack_init(void);

esp_err_t nrf24_mousejack_scan_start(uint32_t duration_s);
esp_err_t nrf24_mousejack_scan_stop(void);
bool      nrf24_mousejack_scan_is_running(void);

void      nrf24_mousejack_dump(void);
uint32_t  nrf24_mousejack_count(void);
void      nrf24_mousejack_clear(void);

esp_err_t nrf24_mousejack_inject(uint32_t target_idx, const char *text);
