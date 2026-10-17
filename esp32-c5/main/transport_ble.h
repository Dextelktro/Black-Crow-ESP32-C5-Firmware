#pragma once

#include <stdbool.h>
#include <stdint.h>

bool transport_ble_init(void);

/* Number of currently active BLE GATT connections (0 if no client paired). */
uint32_t transport_ble_connected_count(void);

/* ============================================================
 * Pause / resume NUS advertising.
 *
 * Active GATT connections stay alive while advertising is paused.
 * Used by the BLE scanner so the radio can focus on discovery.
 * ============================================================ */
void transport_ble_pause_adv(void);
void transport_ble_resume_adv(void);
bool transport_ble_is_adv_paused(void);
