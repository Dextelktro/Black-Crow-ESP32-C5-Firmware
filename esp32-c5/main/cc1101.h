#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t cc1101_init(void);
esp_err_t cc1101_sleep(void);
esp_err_t cc1101_wake(void);
esp_err_t cc1101_deinit(void);
bool cc1101_is_present(void);
bool cc1101_is_sleeping(void);
