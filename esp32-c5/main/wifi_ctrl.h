/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

void wifi_ctrl_init(void);
int  wifi_ctrl_scan(void);
esp_err_t wifi_ctrl_scan_start(void);
esp_err_t wifi_ctrl_scan_stop(void);
bool wifi_ctrl_scan_is_running(void);
void wifi_ctrl_scan_clear(void);
void wifi_ctrl_set_channel(uint8_t channel);

/* Evil-portal radio handoff. The firmware normally runs STA + promiscuous
 * (the TX precondition for every other WiFi feature). A captive portal needs
 * a real SoftAP + LwIP stack, so the radio must switch modes.
 *
 *   wifi_ctrl_ap_enter() : promiscuous OFF -> WIFI_MODE_AP on `channel`
 *                          (2.4 GHz), opens an OPEN AP named `ssid`, and
 *                          points the DHCP-offered DNS at the AP itself so a
 *                          captive-portal DNS hijack works. Gateway/host IP is
 *                          192.168.4.1.
 *   wifi_ctrl_ap_exit()  : AP down -> WIFI_MODE_STA + band AUTO + promiscuous
 *                          ON again, restoring the state every other feature
 *                          expects.
 * Both are idempotent-safe to pair; exit always restores the STA baseline. */
esp_err_t wifi_ctrl_ap_enter(const char *ssid, uint8_t channel);
esp_err_t wifi_ctrl_ap_exit(void);

/* Fast active scan for WiFi live-scan mode.
 *   band_5g    : also scan the 5 GHz band when true
 *   min_ms/max_ms : per-channel active dwell window
 *   stop_flag  : optional pointer; scan aborts early when it becomes true
 * Returns >=0 AP count on success, -1 on failure. Updates the target list. */
int wifi_ctrl_scan_fast(bool band_5g, uint32_t min_ms, uint32_t max_ms,
                        volatile bool *stop_flag);
