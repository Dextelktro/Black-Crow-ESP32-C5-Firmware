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
 * WiFi beacon spam — floods fake APs across 2.4/5 GHz channels.
 * ============================================================ */

typedef enum {
    WSPAM_MODE_FUNNY = 0,
    WSPAM_MODE_RICKROLL,
    WSPAM_MODE_RANDOM,
    WSPAM_MODE_ALL,       /* rotate funny + rickroll + random */
    WSPAM_MODE_CUSTOM,
    WSPAM_MODE_COUNT,
} wspam_mode_t;

typedef enum {
    WSPAM_RATE_AGGRESSIVE = 0,  /* 10 ms  */
    WSPAM_RATE_NORMAL,          /* 20 ms  */
    WSPAM_RATE_STEALTH,         /* 100 ms */
} wspam_rate_t;

esp_err_t wifi_spam_init(void);
esp_err_t wifi_spam_start(wspam_mode_t mode, wspam_rate_t rate,
                          bool band_5g, uint32_t duration_s);
esp_err_t wifi_spam_stop(void);
bool      wifi_spam_is_running(void);

const char *wifi_spam_mode_name(wspam_mode_t mode);
const char *wifi_spam_rate_name(wspam_rate_t rate);

wspam_mode_t wifi_spam_current_mode(void);
wspam_rate_t wifi_spam_current_rate(void);
bool         wifi_spam_current_dual_band(void);
uint32_t     wifi_spam_total_sent(void);
uint32_t     wifi_spam_current_channel(void);
uint32_t     wifi_spam_ssid_count(void);
uint32_t     wifi_spam_elapsed_sec(void);

/* Custom SSID list (NVS-persisted). */
esp_err_t wifi_spam_custom_add(const char *ssid);
esp_err_t wifi_spam_custom_clear(void);
uint32_t  wifi_spam_custom_count(void);
esp_err_t wifi_spam_custom_get(uint32_t idx, char *out, size_t out_size);
