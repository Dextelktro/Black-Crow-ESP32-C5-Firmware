#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Maksimum device yang bisa disimpan di buffer */
#define BLE_SCAN_MAX_DEVICES  128
#define BLE_SCAN_NAME_LEN     32

/* Entry hasil scan */
typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;      /* 0=public, 1=random */
    int8_t   rssi;
    uint8_t  adv_type;       /* legacy PDU type (0=ADV_IND, dll) */
    uint16_t mfg_id;         /* 0xFFFF = tidak ada */
    char     name[BLE_SCAN_NAME_LEN];
    uint8_t  name_len;
    uint16_t seen;           /* jumlah kali terdeteksi */
    uint32_t last_seen_ms;
} ble_scan_entry_t;

esp_err_t ble_scanner_init(void);

/* Mulai scan. duration_sec=0 → scan sampai manual stop */
esp_err_t ble_scanner_start(uint32_t duration_sec, bool active_scan);

/* Stop scan (manual atau durasi habis) */
esp_err_t ble_scanner_stop(void);

bool ble_scanner_is_running(void);

/* List hasil scan ke io_log */
void ble_scanner_dump(void);

/* Bersihkan buffer */
void ble_scanner_clear(void);

/* Info status */
uint32_t ble_scanner_count(void);
uint32_t ble_scanner_elapsed_sec(void);
