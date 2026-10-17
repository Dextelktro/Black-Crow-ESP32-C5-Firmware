#pragma once

#include "esp_err.h"
#include "nrf24_driver.h"
#include "nrf24_scanner.h"
#include "nrf24_jammer.h"
#include "nrf24_sniffer.h"
#include "nrf24_mousejack.h"

esp_err_t nrf24_module_init(void);
bool      nrf24_module_ready(void);
