#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define NRF24_SCAN_CHANNELS  126

typedef struct {
    uint8_t channel;
    uint8_t rpd_count;
    bool    peak;
} nrf24_scan_result_t;

esp_err_t nrf24_scanner_init(void);

esp_err_t nrf24_scanner_waterfall_start(void);
esp_err_t nrf24_scanner_waterfall_stop(void);
bool      nrf24_scanner_waterfall_is_running(void);

int       nrf24_scanner_peak(uint8_t *out_ch);
void      nrf24_scanner_dump(void);

const nrf24_scan_result_t *nrf24_scanner_get_results(void);
