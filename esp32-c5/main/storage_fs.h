/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* ============================================================
 * Shared LittleFS mount for the `storage` partition.
 * ------------------------------------------------------------
 * esp_littlefs allows only ONE mount per partition, but several features
 * (Infrared, Evil Portal) need to persist files there. This module mounts
 * the partition exactly once at STORAGE_FS_BASE and both features build
 * their paths under it:
 *   IR     -> STORAGE_FS_BASE "/<name>.ir"   (root level — unchanged layout)
 *   Portal -> STORAGE_FS_BASE "/portal/..."  (own subdirectory)
 * ============================================================ */

#define STORAGE_FS_BASE "/fs"

/* Mount the partition if it isn't already. Idempotent and safe to call from
 * every feature's init. Returns ESP_OK once mounted. */
esp_err_t storage_fs_mount(void);

bool storage_fs_ready(void);

/* Report LittleFS usage for the `storage` partition. Fills *total and *used
 * (bytes) and returns ESP_OK. Returns ESP_ERR_INVALID_STATE if the partition
 * is not mounted yet. Used by the system telemetry heartbeat to surface the
 * flash storage that remains. */
esp_err_t storage_fs_info(size_t *total, size_t *used);

/* ---- Generic file browser (Home "LittleFS Files" manager) ---------------- */

/* Called once per regular file found while walking the partition. rel_path is
 * the path relative to STORAGE_FS_BASE (e.g. "remote.ir", "portal/creds.log"),
 * never with a leading slash. */
typedef void (*storage_fs_walk_cb)(const char *rel_path, size_t size,
                                   bool is_directory, void *ctx);

/* Recursively walk every file under STORAGE_FS_BASE (all subdirectories),
 * invoking cb for each regular file and directory.
 * Returns ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t storage_fs_walk(storage_fs_walk_cb cb, void *ctx);

esp_err_t storage_fs_create_directory(const char *rel_path);
esp_err_t storage_fs_rename(const char *old_rel_path, const char *new_rel_path);

/* Delete one file or an entire directory tree below STORAGE_FS_BASE. */
esp_err_t storage_fs_delete(const char *rel_path);
