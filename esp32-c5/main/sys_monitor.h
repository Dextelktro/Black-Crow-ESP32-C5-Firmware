/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "esp_err.h"

/* ============================================================
 * System telemetry heartbeat.
 * ------------------------------------------------------------
 * Emits one `sys:` line every 250 ms carrying live device health that the
 * app shows on the Home dashboard:
 *
 *   sys: cpu=<percent> heap=<free>/<total> psram=<free>/<total> flash=<free>/<total>
 *
 *   cpu    CPU utilization calculated from the FreeRTOS idle-task runtime,
 *          averaged over a rolling one-second window and refreshed every
 *          250 ms.
 *   heap   internal SRAM heap, bytes (MALLOC_CAP_INTERNAL).
 *   psram  external PSRAM heap, bytes (MALLOC_CAP_SPIRAM); 0/0 if absent.
 *   flash  LittleFS `storage` partition, bytes free / total.
 * ============================================================ */

esp_err_t sys_monitor_init(void);
