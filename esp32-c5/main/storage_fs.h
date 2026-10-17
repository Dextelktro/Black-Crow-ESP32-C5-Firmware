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
                                    void *ctx);

/* Recursively walk every file under STORAGE_FS_BASE (all subdirectories),
 * invoking cb for each regular file. Directories are descended, not reported.
 * Returns ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t storage_fs_walk(storage_fs_walk_cb cb, void *ctx);

/* Delete one file by its path relative to STORAGE_FS_BASE. Rejects absolute
 * paths and any path containing ".." (no escaping the mount). Returns
 * ESP_ERR_NOT_FOUND if the file does not exist, ESP_ERR_INVALID_ARG on a bad
 * path, ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t storage_fs_delete(const char *rel_path);
