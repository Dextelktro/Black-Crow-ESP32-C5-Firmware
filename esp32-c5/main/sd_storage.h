/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*sd_storage_walk_cb)(const char *path, size_t size, void *ctx);

esp_err_t sd_storage_init(void);
esp_err_t sd_storage_mount(void);
esp_err_t sd_storage_unmount(void);
esp_err_t sd_storage_acquire(void);
esp_err_t sd_storage_release(void);
esp_err_t sd_storage_check_access(void);
esp_err_t sd_storage_format(void);
esp_err_t sd_storage_info(
    uint64_t *total,
    uint64_t *used,
    uint64_t *free_bytes);
esp_err_t sd_storage_walk(sd_storage_walk_cb callback, void *ctx);
esp_err_t sd_storage_copy_to_littlefs(const char *path);
esp_err_t sd_storage_copy_from_littlefs(const char *path);
esp_err_t sd_storage_download_start(const char *path, size_t max_bytes);
esp_err_t sd_storage_download_ack(uint32_t transfer_id, size_t next_offset);
esp_err_t sd_storage_download_cancel(uint32_t transfer_id);
void sd_storage_get_state(bool *present, bool *mounted, bool *busy,
                          bool *radio_safe);
