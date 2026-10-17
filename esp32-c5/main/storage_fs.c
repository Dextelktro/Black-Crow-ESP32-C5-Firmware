#include "storage_fs.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "io.h"

#define STORAGE_PART_LABEL "storage"

static const char *TAG = "storage_fs";
static bool s_ready = false;

bool storage_fs_ready(void) { return s_ready; }

esp_err_t storage_fs_mount(void)
{
    if (s_ready) return ESP_OK;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_FS_BASE,
        .partition_label = STORAGE_PART_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        io_log("storage: littlefs mount failed (%s)\r\n", esp_err_to_name(err));
        return err;
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

static void walk_dir(const char *abs_dir, const char *rel_prefix, int depth,
                     storage_fs_walk_cb cb, void *ctx)
{
    if (depth >= STORAGE_FS_MAX_DEPTH) return;
    DIR *dir = opendir(abs_dir);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        /* abs_dir/rel_prefix are runtime pointers of unknown length, so these
         * snprintf()s can't be proven to truncate (no -Wformat-truncation);
         * oversize buffers keep real, shallow paths intact and truncation is
         * harmless (a too-long path simply fails the stat below). */
        char abs_path[320];
        snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, name);
        char rel_path[288];
        if (rel_prefix[0] != '\0') {
            snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, name);
        } else {
            snprintf(rel_path, sizeof(rel_path), "%s", name);
        }

        struct stat st;
        if (stat(abs_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk_dir(abs_path, rel_path, depth + 1, cb, ctx);
        } else if (cb) {
            cb(rel_path, (size_t)st.st_size, ctx);
        }
    }
    closedir(dir);
}

esp_err_t storage_fs_walk(storage_fs_walk_cb cb, void *ctx)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    walk_dir(STORAGE_FS_BASE, "", 0, cb, ctx);
    return ESP_OK;
}

esp_err_t storage_fs_delete(const char *rel_path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!rel_path || rel_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    /* Stay inside the mount: no absolute paths, no parent-directory escapes. */
    if (rel_path[0] == '/' || strstr(rel_path, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char abs_path[320];
    snprintf(abs_path, sizeof(abs_path), STORAGE_FS_BASE "/%s", rel_path);
    if (remove(abs_path) != 0) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}
