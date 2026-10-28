/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "storage_fs.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "io.h"

#define STORAGE_PART_LABEL "storage"
#define STORAGE_BLANK_CHECK_CHUNK 4096

static const char *TAG = "storage_fs";
static bool s_ready = false;

bool storage_fs_ready(void) { return s_ready; }

static bool storage_partition_is_blank(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
        STORAGE_PART_LABEL);
    if (!partition) {
        io_log("storage: partition lookup failed; refusing to format\r\n");
        return false;
    }

    uint8_t *chunk = heap_caps_malloc(
        STORAGE_BLANK_CHECK_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        io_log("storage: blank check allocation failed; refusing to format\r\n");
        return false;
    }

    for (size_t offset = 0; offset < partition->size;) {
        size_t remaining = partition->size - offset;
        size_t read_size = remaining < STORAGE_BLANK_CHECK_CHUNK
            ? remaining : STORAGE_BLANK_CHECK_CHUNK;
        esp_err_t err = esp_partition_read(partition, offset, chunk, read_size);
        if (err != ESP_OK) {
            io_log("storage: blank check read failed (%s); refusing to format\r\n",
                   esp_err_to_name(err));
            free(chunk);
            return false;
        }
        for (size_t i = 0; i < read_size; i++) {
            if (chunk[i] != 0xFF) {
                free(chunk);
                return false;
            }
        }
        offset += read_size;
    }

    free(chunk);
    return true;
}

esp_err_t storage_fs_mount(void)
{
    if (s_ready) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_FS_BASE,
        .partition_label = STORAGE_PART_LABEL,
        .format_if_mount_failed = false,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        io_log("storage: littlefs mount failed (%s)\r\n",
               esp_err_to_name(err));
        if (!storage_partition_is_blank()) {
            io_log("storage: refusing to format non-empty or unreadable partition\r\n");
            return err;
        }

        io_log("storage: empty partition detected; formatting for first use\r\n");
        err = esp_littlefs_format(STORAGE_PART_LABEL);
        if (err != ESP_OK) {
            io_log("storage: initial format failed (%s)\r\n",
                   esp_err_to_name(err));
            return err;
        }
        err = esp_vfs_littlefs_register(&conf);
        if (err != ESP_OK) {
            io_log("storage: mount after initial format failed (%s)\r\n",
                   esp_err_to_name(err));
            return err;
        }
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(STORAGE_PART_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes",
                 STORAGE_FS_BASE, (unsigned)used, (unsigned)total);
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t storage_fs_info(size_t *total, size_t *used)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return esp_littlefs_info(STORAGE_PART_LABEL, total, used);
}

/* Depth cap so a corrupt/looping tree can never recurse without bound. The
 * real layout is shallow (IR at root, portal one level down). */
#define STORAGE_FS_MAX_DEPTH 6

static bool valid_relative_path(const char *path)
{
    if (!path || path[0] == '\0' || path[0] == '/' ||
        path[strlen(path) - 1] == '/' || strlen(path) >= 256) {
        return false;
    }
    const char *segment = path;
    for (const unsigned char *p = (const unsigned char *)path;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t length = (size_t)((const char *)p - segment);
            if (length == 0 ||
                (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.')) {
                return false;
            }
            if (*p == '\0') break;
            segment = (const char *)p + 1;
        } else if (*p < 0x20 || *p == 0x7f || *p == '\\' ||
                   *p == '"' || *p == ':') {
            return false;
        }
    }
    return true;
}

static esp_err_t walk_dir(const char *abs_dir, const char *rel_prefix, int depth,
                          storage_fs_walk_cb cb, void *ctx)
{
    if (depth > STORAGE_FS_MAX_DEPTH) return ESP_ERR_INVALID_SIZE;
    DIR *dir = opendir(abs_dir);
    if (!dir) return ESP_FAIL;

    struct dirent *de;
    esp_err_t err = ESP_OK;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            if (errno != 0) err = ESP_FAIL;
            break;
        }
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char abs_path[320];
        int abs_len = snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, name);
        char rel_path[288];
        int rel_len = rel_prefix[0] != '\0'
            ? snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, name)
            : snprintf(rel_path, sizeof(rel_path), "%s", name);
        if (abs_len < 0 || (size_t)abs_len >= sizeof(abs_path) ||
            rel_len < 0 || (size_t)rel_len >= sizeof(rel_path)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        struct stat st;
        if (stat(abs_path, &st) != 0) {
            err = ESP_FAIL;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (cb) cb(rel_path, 0, true, ctx);
            err = walk_dir(abs_path, rel_path, depth + 1, cb, ctx);
            if (err != ESP_OK) break;
        } else if (cb) {
            if (st.st_size < 0) {
                err = ESP_FAIL;
                break;
            }
            cb(rel_path, (size_t)st.st_size, false, ctx);
        }
    }
    if (closedir(dir) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t storage_fs_walk(storage_fs_walk_cb cb, void *ctx)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return walk_dir(STORAGE_FS_BASE, "", 0, cb, ctx);
}

static esp_err_t remove_tree(const char *path, int depth)
{
    if (depth > STORAGE_FS_MAX_DEPTH) return ESP_ERR_INVALID_SIZE;
    struct stat info;
    if (stat(path, &info) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISDIR(info.st_mode)) {
        return remove(path) == 0 ? ESP_OK : ESP_FAIL;
    }

    DIR *dir = opendir(path);
    if (!dir) return ESP_FAIL;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) err = ESP_FAIL;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[320];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = remove_tree(child, depth + 1);
        if (err != ESP_OK) break;
    }
    if (closedir(dir) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err != ESP_OK) return err;
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t resolve_path(const char *rel_path, char abs_path[320])
{
    if (!valid_relative_path(rel_path)) return ESP_ERR_INVALID_ARG;
    int n = snprintf(abs_path, 320, STORAGE_FS_BASE "/%s", rel_path);
    return n >= 0 && n < 320 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t storage_fs_create_directory(const char *rel_path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[320];
    esp_err_t err = resolve_path(rel_path, path);
    if (err != ESP_OK) return err;
    if (mkdir(path, 0775) == 0) return ESP_OK;
    return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t storage_fs_rename(const char *old_rel_path, const char *new_rel_path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char old_path[320];
    char new_path[320];
    esp_err_t err = resolve_path(old_rel_path, old_path);
    if (err != ESP_OK) return err;
    err = resolve_path(new_rel_path, new_path);
    if (err != ESP_OK) return err;
    struct stat info;
    if (stat(old_path, &info) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (stat(new_path, &info) == 0 || errno != ENOENT) {
        return ESP_ERR_INVALID_STATE;
    }
    return rename(old_path, new_path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_fs_delete(const char *rel_path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char path[320];
    esp_err_t err = resolve_path(rel_path, path);
    if (err != ESP_OK) return err;
    return remove_tree(path, 0);
}
