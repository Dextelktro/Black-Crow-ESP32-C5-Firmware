#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define NRF24_SNIFF_MAX_PACKETS  64
#define NRF24_SNIFF_PAYLOAD_LEN  32

typedef struct {
    uint8_t  payload[NRF24_SNIFF_PAYLOAD_LEN];
    uint8_t  channel;
    uint8_t  len;
    int8_t   rssi;
    uint32_t timestamp_ms;
    uint32_t hash;
} nrf24_sniff_packet_t;

esp_err_t nrf24_sniffer_init(void);

esp_err_t nrf24_sniffer_start(uint8_t channel, uint32_t duration_s);
esp_err_t nrf24_sniffer_stop(void);
bool      nrf24_sniffer_is_running(void);

void      nrf24_sniffer_dump(void);
uint32_t  nrf24_sniffer_count(void);
esp_err_t nrf24_sniffer_get(uint32_t idx, nrf24_sniff_packet_t *out);
void      nrf24_sniffer_clear(void);

esp_err_t nrf24_sniffer_replay(uint32_t idx, uint32_t count, uint32_t delay_us);
