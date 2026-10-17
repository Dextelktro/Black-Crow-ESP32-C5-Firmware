#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============================================================
 * WiFi channel-energy spectrum.
 *
 * Hops every 2.4 GHz (and optionally 5 GHz) channel in promiscuous
 * mode and reports per-channel peak RSSI + frame activity, streaming
 * a live frequency-domain spectrum to the app. This is an activity
 * spectrum (what the WiFi radio can hear), NOT a raw I/Q SDR capture.
 * ============================================================ */

esp_err_t spectrum_init(void);
esp_err_t spectrum_start(bool include_5ghz);
esp_err_t spectrum_stop(void);
bool      spectrum_is_running(void);
