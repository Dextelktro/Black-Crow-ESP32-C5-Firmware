#include "esp_upload.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_random.h"
#include "esp_system.h"
#include "io.h"
#include "sd_storage.h"
#include "storage_fs.h"

#define UPLOAD_PATH_MAX 192
#define UPLOAD_FILE_BUFFER_SIZE 4096
#define UPLOAD_FILE_FLUSH_SIZE 4096
typedef struct {
    char temporary_path[UPLOAD_PATH_MAX];
    char final_path[UPLOAD_PATH_MAX];
    FILE *open_file;
    uint8_t last_chunk[ESP_UPLOAD_CHUNK_MAX];
    uint32_t id;
    uint32_t expected_size;
    uint32_t expected_crc;
    uint32_t running_crc;
    uint32_t next_offset;
    uint32_t last_offset;
    uint32_t unflushed_bytes;
    size_t last_size;
    esp_flasher_storage_t storage;
    bool storage_reserved;
    bool active;
} upload_state_t;

static upload_state_t s_upload;
static uint32_t s_last_transfer_id;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return crc;
}

static bool valid_relative_path(const char *path)
{
    if (!path || path[0] == '\0' || path[0] == '/' ||
        path[strlen(path) - 1] == '/' || strlen(path) >= UPLOAD_PATH_MAX) {
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

static esp_err_t available_bytes(
    esp_flasher_storage_t storage,
    uint64_t *free_bytes)
{
    uint64_t total = 0;
    uint64_t used = 0;
    if (storage == ESP_FLASHER_STORAGE_LITTLEFS) {
        size_t lfs_total = 0;
        size_t lfs_used = 0;
        esp_err_t err = storage_fs_mount();
        if (err != ESP_OK) return err;
        err = storage_fs_info(&lfs_total, &lfs_used);
        if (err != ESP_OK) return err;
        total = lfs_total;
        used = lfs_used;
    } else {
        esp_err_t err = sd_storage_info(&total, &used, free_bytes);
        if (err != ESP_OK) return err;
        return ESP_OK;
    }
    *free_bytes = total > used ? total - used : 0;
    return ESP_OK;
}

static void reset_state(void)
{
    memset(&s_upload, 0, sizeof(s_upload));
}

static esp_err_t acquire_storage(
    esp_flasher_storage_t storage,
    bool *reserved)
{
    *reserved = false;
    if (storage != ESP_FLASHER_STORAGE_SD) return ESP_OK;
    esp_err_t err = sd_storage_acquire();
    if (err == ESP_OK) *reserved = true;
    return err;
}

static esp_err_t release_storage(bool *reserved)
{
    if (!*reserved) return ESP_OK;
    esp_err_t err = sd_storage_release();
    *reserved = false;
    if (err != ESP_OK) {
        io_log("espflash: SD release failed (%s)\r\n", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t ensure_directory_tree(const char *path)
{
    char current[UPLOAD_PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof(current)) return ESP_ERR_INVALID_SIZE;
    memcpy(current, path, length + 1);

    for (char *part = current + 1; *part; part++) {
        if (*part != '/') continue;
        *part = '\0';
        struct stat info;
        if (stat(current, &info) == 0) {
            if (!S_ISDIR(info.st_mode)) {
                *part = '/';
                return ESP_ERR_INVALID_STATE;
            }
        } else if (errno == ENOENT) {
            if (mkdir(current, 0775) != 0) {
                if (errno != EEXIST || stat(current, &info) != 0 ||
                    !S_ISDIR(info.st_mode)) {
                    *part = '/';
                    return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
                }
            }
        } else {
            *part = '/';
            return ESP_FAIL;
        }
        *part = '/';
    }
    return ESP_OK;
}

static esp_err_t storage_prefix(
    esp_flasher_storage_t storage,
    char prefix[16])
{
    if (storage == ESP_FLASHER_STORAGE_LITTLEFS) {
        esp_err_t err = storage_fs_mount();
        if (err != ESP_OK) return err;
        int n = snprintf(prefix, 16, "%s/", STORAGE_FS_BASE);
        return n >= 0 && n < 16 ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (storage == ESP_FLASHER_STORAGE_SD) {
        memcpy(prefix, "/sdcard/", sizeof("/sdcard/"));
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t esp_upload_create_directory(
    esp_flasher_storage_t storage,
    const char *relative_path)
{
    if (!valid_relative_path(relative_path)) return ESP_ERR_INVALID_ARG;
    if (s_upload.active || esp_flasher_is_busy()) return ESP_ERR_INVALID_STATE;
    char prefix[16];
    esp_err_t err = storage_prefix(storage, prefix);
    if (err != ESP_OK) return err;

    char path[UPLOAD_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s%s", prefix, relative_path);
    if (n < 0 || (size_t)n >= sizeof(path)) return ESP_ERR_INVALID_SIZE;

    bool reserved = false;
    err = acquire_storage(storage, &reserved);
    if (err != ESP_OK) return err;
    err = ensure_directory_tree(path);
    if (err == ESP_OK) {
        struct stat info;
        if (stat(path, &info) == 0) {
            err = S_ISDIR(info.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
        } else if (errno == ENOENT) {
            err = mkdir(path, 0775) == 0 ? ESP_OK : ESP_FAIL;
        } else {
            err = ESP_FAIL;
        }
    }
    esp_err_t release_err = release_storage(&reserved);
    return release_err == ESP_OK ? err : release_err;
}

static esp_err_t discard_upload(void)
{
    esp_err_t result = ESP_OK;
    if (s_upload.open_file) {
        if (fclose(s_upload.open_file) != 0) {
            io_log("espflash: temporary-file close failed during cleanup\r\n");
            result = ESP_FAIL;
        }
        s_upload.open_file = NULL;
    }
    bool reserved = s_upload.storage_reserved;
    s_upload.storage_reserved = false;
    esp_err_t acquire_err = reserved
        ? ESP_OK : acquire_storage(s_upload.storage, &reserved);
    if (acquire_err == ESP_OK) {
        if (s_upload.temporary_path[0] != '\0' &&
            remove(s_upload.temporary_path) != 0 && errno != ENOENT) {
            io_log("espflash: temporary-file cleanup failed (%s)\r\n",
                   s_upload.temporary_path);
            result = ESP_FAIL;
        }
    } else {
        io_log("espflash: cannot acquire storage for upload cleanup (%s)\r\n",
               esp_err_to_name(acquire_err));
        result = acquire_err;
    }
    s_last_transfer_id = s_upload.id;
    esp_err_t release_err = release_storage(&reserved);
    if (release_err != ESP_OK) result = release_err;
    reset_state();
    return result;
}

esp_err_t esp_upload_begin(
    esp_flasher_storage_t storage,
    const char *relative_path,
    uint32_t total_size,
    uint32_t expected_crc,
    uint32_t *transfer_id)
{
    if (!transfer_id ||
        (storage != ESP_FLASHER_STORAGE_LITTLEFS &&
         storage != ESP_FLASHER_STORAGE_SD) ||
        !valid_relative_path(relative_path) ||
        total_size > LONG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_upload.active || esp_flasher_is_busy()) return ESP_ERR_INVALID_STATE;

    char prefix[16];
    esp_err_t err = storage_prefix(storage, prefix);
    if (err != ESP_OK) return err;

    upload_state_t state = {
        .storage = storage,
        .expected_size = total_size,
        .expected_crc = expected_crc,
        .running_crc = UINT32_MAX,
    };
    int path_length = snprintf(state.final_path, sizeof(state.final_path),
                               "%s%s", prefix, relative_path);
    if (path_length < 0 || (size_t)path_length >= sizeof(state.final_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    state.id = esp_random();
    if (state.id == 0 || state.id == s_last_transfer_id) {
        state.id = s_last_transfer_id + 1;
        if (state.id == 0) state.id = 1;
    }
    path_length = snprintf(state.temporary_path, sizeof(state.temporary_path),
                           "%s.%08" PRIx32 ".part", state.final_path, state.id);
    if (path_length < 0 || (size_t)path_length >= sizeof(state.temporary_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t free_bytes = 0;
    err = available_bytes(storage, &free_bytes);
    if (err != ESP_OK) return err;
    if (total_size > free_bytes) return ESP_ERR_NO_MEM;

    bool reserved = false;
    err = acquire_storage(storage, &reserved);
    if (err != ESP_OK) return err;

    err = ensure_directory_tree(state.final_path);
    if (err != ESP_OK) {
        esp_err_t release_err = release_storage(&reserved);
        return release_err == ESP_OK ? err : release_err;
    }

    struct stat file_stat;
    if (stat(state.final_path, &file_stat) == 0) {
        esp_err_t release_err = release_storage(&reserved);
        return release_err == ESP_OK ? ESP_ERR_INVALID_STATE : release_err;
    }
    if (errno != ENOENT) {
        esp_err_t release_err = release_storage(&reserved);
        return release_err == ESP_OK ? ESP_FAIL : release_err;
    }

    if (stat(state.temporary_path, &file_stat) == 0 || errno != ENOENT) {
        esp_err_t release_err = release_storage(&reserved);
        return release_err == ESP_OK ? ESP_FAIL : release_err;
    }
    FILE *file = fopen(state.temporary_path, "wb");
    if (!file) {
        esp_err_t release_err = release_storage(&reserved);
        return release_err == ESP_OK ? ESP_FAIL : release_err;
    }
    bool file_ok = setvbuf(file, NULL, _IOFBF, UPLOAD_FILE_BUFFER_SIZE) == 0;
    if (!file_ok) {
        if (fclose(file) != 0) {
            io_log("espflash: temporary-file close failed during upload setup\r\n");
        }
        if (remove(state.temporary_path) != 0 && errno != ENOENT) {
            io_log("espflash: could not remove incomplete upload %s\r\n",
                   state.temporary_path);
        }
        esp_err_t release_err = release_storage(&reserved);
        return release_err == ESP_OK ? ESP_FAIL : release_err;
    }
    state.open_file = file;

    state.active = true;
    if (storage == ESP_FLASHER_STORAGE_SD) {
        state.storage_reserved = reserved;
        reserved = false;
    } else {
        err = release_storage(&reserved);
        if (err != ESP_OK) {
            if (state.open_file && fclose(state.open_file) != 0) {
                io_log("espflash: temporary-file close failed during setup cleanup\r\n");
            }
            if (remove(state.temporary_path) != 0 && errno != ENOENT) {
                io_log("espflash: could not remove incomplete upload %s\r\n",
                       state.temporary_path);
            }
            return err;
        }
    }
    s_upload = state;
    *transfer_id = s_upload.id;
    return ESP_OK;
}

esp_err_t esp_upload_write(
    uint32_t transfer_id,
    uint32_t offset,
    const uint8_t *data,
    size_t size,
    uint32_t *next_offset)
{
    if (!s_upload.active || transfer_id != s_upload.id ||
        !data || !next_offset || size == 0 || size > ESP_UPLOAD_CHUNK_MAX ||
        (uint64_t)offset + size > s_upload.expected_size) {
        return ESP_ERR_INVALID_ARG;
    }

    if (offset == s_upload.last_offset && size == s_upload.last_size &&
        offset + size == s_upload.next_offset &&
        memcmp(s_upload.last_chunk, data, size) == 0) {
        *next_offset = s_upload.next_offset;
        return ESP_OK;
    }
    if (offset != s_upload.next_offset) return ESP_ERR_INVALID_STATE;

    if (s_upload.storage == ESP_FLASHER_STORAGE_SD) {
        esp_err_t access_err = sd_storage_check_access();
        if (access_err != ESP_OK) {
            esp_err_t cleanup_err = discard_upload();
            if (cleanup_err != ESP_OK) {
                io_log("espflash: upload cleanup failed (%s)\r\n",
                       esp_err_to_name(cleanup_err));
            }
            return access_err;
        }
    }
    if (!s_upload.open_file ||
        fwrite(data, 1, size, s_upload.open_file) != size) {
        esp_err_t cleanup_err = discard_upload();
        if (cleanup_err != ESP_OK) {
            io_log("espflash: upload cleanup failed (%s)\r\n",
                   esp_err_to_name(cleanup_err));
        }
        return ESP_FAIL;
    }
    s_upload.unflushed_bytes += (uint32_t)size;
    if (s_upload.unflushed_bytes >= UPLOAD_FILE_FLUSH_SIZE) {
        if (fflush(s_upload.open_file) != 0) {
            esp_err_t cleanup_err = discard_upload();
            if (cleanup_err != ESP_OK) {
                io_log("espflash: upload cleanup failed (%s)\r\n",
                       esp_err_to_name(cleanup_err));
            }
            return ESP_FAIL;
        }
        s_upload.unflushed_bytes = 0;
    }
    s_upload.running_crc = crc32_update(s_upload.running_crc, data, size);
    memcpy(s_upload.last_chunk, data, size);
    s_upload.last_offset = offset;
    s_upload.last_size = size;
    s_upload.next_offset += (uint32_t)size;
    *next_offset = s_upload.next_offset;
    return ESP_OK;
}

esp_err_t esp_upload_finish(uint32_t transfer_id)
{
    if (!s_upload.active || transfer_id != s_upload.id) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_upload.next_offset != s_upload.expected_size ||
        ~s_upload.running_crc != s_upload.expected_crc) {
        esp_err_t cleanup_err = discard_upload();
        if (cleanup_err != ESP_OK) {
            io_log("espflash: upload cleanup failed (%s)\r\n",
                   esp_err_to_name(cleanup_err));
        }
        return ESP_ERR_INVALID_CRC;
    }

    if (s_upload.storage == ESP_FLASHER_STORAGE_SD) {
        esp_err_t access_err = sd_storage_check_access();
        if (access_err != ESP_OK) {
            esp_err_t cleanup_err = discard_upload();
            if (cleanup_err != ESP_OK) {
                io_log("espflash: upload cleanup failed (%s)\r\n",
                       esp_err_to_name(cleanup_err));
            }
            return access_err;
        }
    }

    if (s_upload.open_file) {
        FILE *file = s_upload.open_file;
        s_upload.open_file = NULL;
        if (fflush(file) != 0) {
            (void)fclose(file);
            esp_err_t cleanup_err = discard_upload();
            if (cleanup_err != ESP_OK) {
                io_log("espflash: upload cleanup failed (%s)\r\n",
                       esp_err_to_name(cleanup_err));
            }
            return ESP_FAIL;
        }
        if (fclose(file) != 0) {
            esp_err_t cleanup_err = discard_upload();
            if (cleanup_err != ESP_OK) {
                io_log("espflash: upload cleanup failed (%s)\r\n",
                       esp_err_to_name(cleanup_err));
            }
            return ESP_FAIL;
        }
    }

    bool reserved = s_upload.storage_reserved;
    s_upload.storage_reserved = false;
    esp_err_t err = reserved
        ? ESP_OK : acquire_storage(s_upload.storage, &reserved);
    if (err != ESP_OK) {
        esp_err_t cleanup_err = discard_upload();
        if (cleanup_err != ESP_OK) {
            io_log("espflash: upload cleanup failed (%s)\r\n",
                   esp_err_to_name(cleanup_err));
        }
        return err;
    }

    FILE *verify = fopen(s_upload.temporary_path, "rb");
    if (!verify) {
        err = ESP_FAIL;
        goto fail;
    }
    if (setvbuf(verify, NULL, _IONBF, 0) != 0) {
        err = ESP_FAIL;
        goto fail;
    }
    uint32_t verified_crc = UINT32_MAX;
    uint32_t verified_size = 0;
    size_t read_size = 0;
    esp_err_t access_err = ESP_OK;
    bool read_failed = false;
    do {
        if (s_upload.storage == ESP_FLASHER_STORAGE_SD) {
            access_err = sd_storage_check_access();
            if (access_err != ESP_OK) {
                read_failed = true;
                break;
            }
        }
        read_size = fread(s_upload.last_chunk, 1, sizeof(s_upload.last_chunk), verify);
        if (read_size > 0) {
            verified_crc = crc32_update(
                verified_crc, s_upload.last_chunk, read_size);
            verified_size += (uint32_t)read_size;
        }
    } while (read_size == sizeof(s_upload.last_chunk));
    if (ferror(verify) != 0) read_failed = true;
    if (fclose(verify) != 0) read_failed = true;
    verify = NULL;
    if (read_failed) {
        err = access_err != ESP_OK ? access_err : ESP_FAIL;
        goto fail;
    }
    if (verified_size != s_upload.expected_size ||
        ~verified_crc != s_upload.expected_crc) {
        io_log("espflash: uploaded file read-back verification failed\r\n");
        err = ESP_ERR_INVALID_CRC;
        goto fail;
    }

    if (s_upload.storage == ESP_FLASHER_STORAGE_SD) {
        err = sd_storage_check_access();
        if (err != ESP_OK) goto fail;
    }

    struct stat existing;
    if (stat(s_upload.final_path, &existing) == 0 || errno != ENOENT) {
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    if (rename(s_upload.temporary_path, s_upload.final_path) != 0) {
        io_log("espflash: could not promote temporary upload %s\r\n",
               s_upload.temporary_path);
        if (remove(s_upload.temporary_path) != 0 && errno != ENOENT) {
            io_log("espflash: could not remove incomplete upload %s\r\n",
                   s_upload.temporary_path);
        }
        err = ESP_FAIL;
        goto finish;
    }

    s_last_transfer_id = s_upload.id;
    err = release_storage(&reserved);
    reset_state();
    return err;

fail:
    if (verify && fclose(verify) != 0) {
        io_log("espflash: temporary-file close failed during verification\r\n");
    }
    {
        esp_err_t release_err = release_storage(&reserved);
        if (release_err != ESP_OK) err = release_err;
    }
    {
        esp_err_t cleanup_err = discard_upload();
        if (cleanup_err != ESP_OK) {
            io_log("espflash: upload cleanup failed (%s)\r\n",
                   esp_err_to_name(cleanup_err));
        }
    }
    return err;

finish:
    s_last_transfer_id = s_upload.id;
    {
        esp_err_t release_err = release_storage(&reserved);
        if (release_err != ESP_OK) err = release_err;
    }
    reset_state();
    return err;
}

esp_err_t esp_upload_cancel(uint32_t transfer_id)
{
    if (transfer_id == 0) return ESP_ERR_INVALID_ARG;
    if (!s_upload.active) {
        return transfer_id == s_last_transfer_id ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (transfer_id != s_upload.id) return ESP_ERR_INVALID_STATE;
    return discard_upload();
}

esp_err_t esp_upload_cancel_active(void)
{
    if (!s_upload.active) return ESP_ERR_INVALID_STATE;
    return discard_upload();
}

bool esp_upload_is_busy(void)
{
    return s_upload.active;
}

bool esp_upload_get_status(
    uint32_t *transfer_id,
    uint32_t *offset,
    uint32_t *total_size)
{
    if (!s_upload.active) return false;
    if (transfer_id) *transfer_id = s_upload.id;
    if (offset) *offset = s_upload.next_offset;
    if (total_size) *total_size = s_upload.expected_size;
    return true;
}
