#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAC_LEN 6

extern const uint8_t MAC_BROADCAST[MAC_LEN];

void frames_send_deauth(const uint8_t *da, const uint8_t *bssid, uint16_t reason);
void frames_send_disassoc(const uint8_t *da, const uint8_t *bssid, uint16_t reason);
bool frames_send_deauth_broadcast(const uint8_t *bssid, uint16_t reason);
void frames_send_auth_flood(const uint8_t *bssid);

uint32_t frames_burst_deauth(uint8_t channel, const uint8_t *da, const uint8_t *bssid);
uint32_t frames_burst_disassoc(uint8_t channel, const uint8_t *da, const uint8_t *bssid);
uint32_t frames_burst_auth_flood(uint8_t channel, const uint8_t *bssid);

uint32_t frames_get_tx_ok_count(void);
uint32_t frames_get_tx_fail_count(void);
void     frames_reset_tx_counters(void);
