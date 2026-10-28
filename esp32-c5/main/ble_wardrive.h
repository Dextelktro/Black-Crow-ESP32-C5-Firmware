/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* BLE wardriving — continuous promiscuous-style BLE discovery that streams
 * devices in real time (NEW / UPD / GONE), classifies them by vendor, device
 * type and MAC-privacy class, and ages out devices no longer heard.
 *
 * It shares the single NimBLE discovery slot with the BLE scanner / spam /
 * spectrum, so starting one stops the others. */

esp_err_t ble_wardrive_init(void);

/* Start continuous wardriving. active=true uses active scanning (sends scan
 * requests → scan responses, which carry device names) at the cost of being
 * more detectable. duration_sec=0 → run until stopped. */
esp_err_t ble_wardrive_start(bool active, uint32_t duration_sec);

esp_err_t ble_wardrive_stop(void);
bool      ble_wardrive_is_running(void);

uint32_t  ble_wardrive_total_unique(void);
uint32_t  ble_wardrive_cycle_count(void);

/* Append one currently observed Apple Find My candidate to LittleFS. */
esp_err_t ble_wardrive_save_findmy(const char *address);
