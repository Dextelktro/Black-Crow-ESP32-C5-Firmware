#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============================================================
 * Channel monitor — standalone passive profiler for one AP.
 *
 * Locks the radio to the AP's channel, enables a promiscuous RX
 * callback filtered to that BSSID and streams live stats:
 *   - RSSI (last observed)
 *   - FPS  (frames per second seen for this BSSID)
 *   - total / mgmt / data frame counts
 *   - distinct STA count
 *
 * Only one monitor runs at a time. Refuses to start while a
 * WiFi attack (nuke / deauth-all / mixed) is running.
 * ============================================================ */

esp_err_t monitor_init(void);

/* Start monitoring the AP at `ap_idx` in the scanned AP table. */
esp_err_t monitor_start(uint16_t ap_idx);

/* Stop the active monitor (no-op if idle). */
esp_err_t monitor_stop(void);

bool monitor_is_running(void);
