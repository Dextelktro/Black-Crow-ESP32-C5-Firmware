/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_flasher.h"

#define ESP_UPLOAD_CHUNK_MAX 108

esp_err_t esp_upload_create_directory(
    esp_flasher_storage_t storage,
    const char *relative_path);
esp_err_t esp_upload_begin(
    esp_flasher_storage_t storage,
    const char *relative_path,
    uint32_t total_size,
    uint32_t expected_crc,
    uint32_t *transfer_id);
esp_err_t esp_upload_write(
    uint32_t transfer_id,
    uint32_t offset,
    const uint8_t *data,
    size_t size,
    uint32_t *next_offset);
esp_err_t esp_upload_finish(uint32_t transfer_id);
esp_err_t esp_upload_cancel(uint32_t transfer_id);
esp_err_t esp_upload_cancel_active(void);
bool esp_upload_is_busy(void);
bool esp_upload_get_status(uint32_t *transfer_id, uint32_t *offset,
                           uint32_t *total_size);
