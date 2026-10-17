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
const char *wifi_live_scan_mode_name(wlscan_mode_t mode);
uint32_t wifi_live_scan_total_unique(void);
uint32_t wifi_live_scan_cycle_count(void);
