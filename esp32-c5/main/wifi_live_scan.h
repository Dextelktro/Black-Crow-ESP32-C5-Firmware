/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WLSCAN_MODE_FAST = 0,
    WLSCAN_MODE_BALANCED,
    WLSCAN_MODE_DEEP,
} wlscan_mode_t;

esp_err_t wifi_live_scan_init(void);
esp_err_t wifi_live_scan_start(wlscan_mode_t mode,
                               uint32_t interval_ms,
                               uint32_t duration_sec);
esp_err_t wifi_live_scan_stop(void);
bool      wifi_live_scan_is_running(void);

/* One-shot passive sweep for the Network Target screen (the `scan` command).
 * Drives the same reliable, dual-band, BLE-coexistence-safe radio path as the
 * continuous live scan, but bounded and SILENT (no NEW/UPD/GONE/AIR telemetry),
 * mirroring the captured APs into the shared targets table exactly once.
 *   include_5g : also sweep the 5 GHz UNII channels
 *   passes     : number of full channel sweeps (>=1)
 *   dwell_ms   : per-channel listen window (beacons arrive ~every 100 ms)
 *   stop_flag  : optional; aborts the sweep early when it becomes true
 * Returns the AP count, -1 on setup failure, or -2 if aborted. Must NOT run
 * while the continuous live scan is running. */
int wifi_live_scan_snapshot(bool include_5g, int passes, uint32_t dwell_ms,
                            volatile bool *stop_flag);
const char *wifi_live_scan_mode_name(wlscan_mode_t mode);
uint32_t wifi_live_scan_total_unique(void);
uint32_t wifi_live_scan_cycle_count(void);
