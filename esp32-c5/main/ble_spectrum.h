/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============================================================
 * BLE signal-strength spectrum.
 *
 * NimBLE does not expose per-channel RSSI, so a true per-frequency
 * BLE spectrum isn't possible. Instead this runs the observer scan
 * and streams a live RSSI-level histogram (10 buckets from ~-100 to
 * -40 dBm) plus peak/activity, giving a real-time BLE signal spectrum.
 * ============================================================ */

#define BLE_SPECTRUM_BINS 10

esp_err_t ble_spectrum_init(void);
esp_err_t ble_spectrum_start(void);
esp_err_t ble_spectrum_stop(void);
bool      ble_spectrum_is_running(void);
