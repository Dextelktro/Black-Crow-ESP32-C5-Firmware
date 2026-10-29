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
