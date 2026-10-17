#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    BLE_SPAM_APPLE_JUICE = 0,
    BLE_SPAM_SOUR_APPLE,
    BLE_SPAM_SAMSUNG,
    BLE_SPAM_GOOGLE_FASTPAIR,
    BLE_SPAM_WINDOWS_SWIFTPAIR,
    BLE_SPAM_XIAOMI_MIBEACON,
    BLE_SPAM_ANDROID,
    BLE_SPAM_ALL,
    BLE_SPAM_TYPE_COUNT
} ble_spam_type_t;

esp_err_t ble_spam_init(void);
esp_err_t ble_spam_start(ble_spam_type_t type, uint32_t delay_ms,
                         uint32_t duration_sec);
esp_err_t ble_spam_stop(void);
esp_err_t ble_spam_set_delay(uint32_t delay_ms);
bool      ble_spam_is_running(void);
ble_spam_type_t ble_spam_current_type(void);
uint32_t  ble_spam_current_delay(void);
uint32_t  ble_spam_frames_sent(void);
uint32_t  ble_spam_default_delay(ble_spam_type_t type);
const char *ble_spam_type_name(ble_spam_type_t type);
