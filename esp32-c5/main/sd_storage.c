#include "sd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "io.h"
#include "spi_shared_bus.h"
#include "storage_fs.h"

#define SD_CD_GPIO GPIO_NUM_0
#define SD_CS_GPIO GPIO_NUM_1
#define SD_MOUNT_POINT "/sdcard"
#define SD_SPI_CLOCK_KHZ 20000
#define SD_COPY_BUFFER_SIZE 512
#define SD_PATH_MAX 128
#define SD_WALK_MAX_DEPTH 6
#define SD_TRANSFER_CHUNK 64

static const char *TAG = "sd_storage";
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static volatile bool s_mounted;
static volatile bool s_busy;
static volatile bool s_radio_safe;
static volatile bool s_card_removed;
static TaskHandle_t s_transfer_task;
static uint32_t s_transfer_id;
static size_t s_transfer_ack_offset;
static bool s_transfer_ack_pending;
static size_t s_transfer_limit;
static char s_transfer_path[SD_PATH_MAX];
static bool s_transfer_cancelled;

static void card_detect_isr(void *arg)
{
    (void)arg;
    bool present = gpio_get_level(SD_CD_GPIO) == 0;
    if (!present) {
        s_card_removed = true;
    }
    s_radio_safe = !present;
    spi_shared_bus_set_radio_access(!present);
}

static bool card_present(void)
{
    return gpio_get_level(SD_CD_GPIO) == 0;
}

static bool valid_relative_path(const char *path)
{
    if (!path || path[0] == '\0' || path[0] == '/' ||
        strlen(path) >= SD_PATH_MAX) {
        return false;
    }
    if (strstr(path, "..") || strchr(path, '\\')) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == '"' || *p == ':') {
            return false;
        }
    }
    return true;
}

static esp_err_t lock_idle(void)
{
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_card_removed || (s_mounted && !card_present())) {
        if (s_mounted && s_card) {
            esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
            if (err != ESP_OK) {
                xSemaphoreGive(s_mutex);
                return err;
            }
            s_card = NULL;
            s_mounted = false;
        }
        s_card_removed = false;
        s_radio_safe = !card_present();
        spi_shared_bus_set_radio_access(s_radio_safe);
    }

    s_busy = true;
    return ESP_OK;
}

static void unlock_idle(void)
{
    s_busy = false;
    xSemaphoreGive(s_mutex);
}

static bool transfer_should_stop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool stop = s_transfer_cancelled || s_card_removed;
    xSemaphoreGive(s_mutex);
    return stop;
}

static uint32_t transfer_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static esp_err_t mount_locked(bool format_if_mount_failed)
{
    if (s_mounted) {
        s_radio_safe = true;
        spi_shared_bus_set_radio_access(true);
        return ESP_OK;
    }
    if (!card_present()) {
        s_radio_safe = true;
        spi_shared_bus_set_radio_access(true);
        return ESP_ERR_NOT_FOUND;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_shared_bus_host();
    host.max_freq_khz = SD_SPI_CLOCK_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = host.slot;
    slot.gpio_cs = SD_CS_GPIO;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        s_radio_safe = false;
        spi_shared_bus_set_radio_access(false);
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    s_radio_safe = true;
    spi_shared_bus_set_radio_access(true);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sd_storage_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!spi_shared_bus_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t cd_config = {
        .pin_bit_mask = 1ULL << SD_CD_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cd_config);
    if (err != ESP_OK) {
        return err;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    s_radio_safe = !card_present();
    spi_shared_bus_set_radio_access(s_radio_safe);
    esp_err_t isr_err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_initialized = false;
        s_radio_safe = false;
        spi_shared_bus_set_radio_access(false);
        return isr_err;
    }
    err = gpio_isr_handler_add(SD_CD_GPIO, card_detect_isr, NULL);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_initialized = false;
        s_radio_safe = false;
        spi_shared_bus_set_radio_access(false);
        return err;
    }

    ESP_LOGI(TAG, "free heap before mount: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!card_present()) {
        io_log("sd: card not detected (CD active-low)\r\n");
        return ESP_ERR_NOT_FOUND;
    }

    err = lock_idle();
    if (err == ESP_OK) {
        err = mount_locked(false);
        unlock_idle();
    }
    ESP_LOGI(TAG, "free heap after mount: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return err;
}

esp_err_t sd_storage_mount(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    err = mount_locked(false);
    unlock_idle();
    return err;
}

esp_err_t sd_storage_unmount(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted || !s_card) {
        unlock_idle();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        s_mounted = false;
        s_radio_safe = true;
        spi_shared_bus_set_radio_access(true);
    }
    unlock_idle();
    return err;
}

esp_err_t sd_storage_acquire(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = lock_idle();
    if (err != ESP_OK) return err;
    if (!s_mounted || !s_card) {
        unlock_idle();
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_release(void)
{
    if (!s_initialized || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_busy) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (s_card_removed || (s_mounted && !card_present())) {
        if (s_mounted && s_card) {
            err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
            if (err == ESP_OK) {
                s_card = NULL;
                s_mounted = false;
            } else {
                ESP_LOGE(TAG, "unmount after exclusive access failed: %s",
                         esp_err_to_name(err));
            }
        }
        s_card_removed = false;
        s_radio_safe = !card_present();
        spi_shared_bus_set_radio_access(s_radio_safe);
    }

    s_busy = false;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sd_storage_check_access(void)
{
    if (!s_initialized || !s_mutex || !s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_card_removed || !card_present()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_mounted || !s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t sd_storage_format(void)
{
    if (!s_initialized || !s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted) {
        err = mount_locked(true);
        if (err != ESP_OK) {
            unlock_idle();
            return err;
        }
    }
    if (!s_mounted || !s_card || !card_present()) {
        unlock_idle();
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    err = esp_vfs_fat_sdcard_format_cfg(SD_MOUNT_POINT, s_card, &config);
    unlock_idle();
    return err;
}

esp_err_t sd_storage_info(
    uint64_t *total,
    uint64_t *used,
    uint64_t *free_bytes)
{
    if (!total || !used || !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted || !card_present()) {
        unlock_idle();
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_vfs_fat_info(SD_MOUNT_POINT, total, free_bytes);
    if (err == ESP_OK) {
        *used = *total >= *free_bytes ? *total - *free_bytes : 0;
    }
    unlock_idle();
    return err;
}

static esp_err_t walk_dir(const char *abs_dir, const char *rel_prefix,
                          int depth, sd_storage_walk_cb callback, void *ctx)
{
    if (depth >= SD_WALK_MAX_DEPTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    DIR *dir = opendir(abs_dir);
    if (!dir) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    struct dirent *entry;
    for (;;) {
        if (s_card_removed || !card_present()) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                err = ESP_FAIL;
            }
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }
        char abs_path[SD_PATH_MAX + sizeof(SD_MOUNT_POINT)];
        char rel_path[SD_PATH_MAX];
        int abs_len = snprintf(abs_path, sizeof(abs_path), "%s/%s",
                               abs_dir, entry->d_name);
        int rel_len = rel_prefix[0]
            ? snprintf(rel_path, sizeof(rel_path), "%s/%s",
                       rel_prefix, entry->d_name)
            : snprintf(rel_path, sizeof(rel_path), "%s", entry->d_name);
        if (abs_len < 0 || (size_t)abs_len >= sizeof(abs_path) ||
            rel_len < 0 || (size_t)rel_len >= sizeof(rel_path)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        struct stat info;
        if (stat(abs_path, &info) != 0) {
            err = ESP_FAIL;
            break;
        }
        if (S_ISDIR(info.st_mode)) {
            err = walk_dir(abs_path, rel_path, depth + 1, callback, ctx);
            if (err != ESP_OK) {
                break;
            }
        } else if (callback) {
            if (info.st_size < 0) {
                err = ESP_FAIL;
                break;
            }
            callback(rel_path, (size_t)info.st_size, ctx);
        }
    }
    if (closedir(dir) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    return err;
}

esp_err_t sd_storage_walk(sd_storage_walk_cb callback, void *ctx)
{
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted || !card_present()) {
        unlock_idle();
        return ESP_ERR_INVALID_STATE;
    }
    err = walk_dir(SD_MOUNT_POINT, "", 0, callback, ctx);
    unlock_idle();
    return err;
}

static bool ensure_parent_dirs(const char *abs_path)
{
    char path[SD_PATH_MAX + sizeof(SD_MOUNT_POINT)];
    size_t len = strlen(abs_path);
    if (len >= sizeof(path)) {
        return false;
    }
    memcpy(path, abs_path, len + 1);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(path, 0775) != 0 && errno != EEXIST) {
            return false;
        }
        *p = '/';
    }
    return true;
}

static uint8_t *copy_buffer_alloc(void)
{
    uint8_t *buffer = heap_caps_malloc(
        SD_COPY_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = heap_caps_malloc(
            SD_COPY_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buffer;
}

static esp_err_t copy_file(const char *src_path, const char *dst_path)
{
    FILE *src = fopen(src_path, "rb");
    if (!src) {
        return ESP_ERR_NOT_FOUND;
    }
    char temp_path[SD_PATH_MAX + sizeof(SD_MOUNT_POINT) + 8];
    int temp_len = snprintf(temp_path, sizeof(temp_path), "%s.part", dst_path);
    if (temp_len < 0 || (size_t)temp_len >= sizeof(temp_path) ||
        !ensure_parent_dirs(temp_path)) {
        fclose(src);
        return ESP_ERR_INVALID_ARG;
    }
    FILE *dst = fopen(temp_path, "wb");
    if (!dst) {
        fclose(src);
        return ESP_FAIL;
    }
    uint8_t *buffer = copy_buffer_alloc();
    if (!buffer) {
        fclose(src);
        fclose(dst);
        remove(temp_path);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    size_t read_count;
    while ((read_count = fread(buffer, 1, SD_COPY_BUFFER_SIZE, src)) > 0) {
        if (s_card_removed || !card_present()) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        if (fwrite(buffer, 1, read_count, dst) != read_count) {
            err = ESP_FAIL;
            break;
        }
    }
    if (ferror(src) || fflush(dst) != 0) {
        err = ESP_FAIL;
    }
    free(buffer);
    int src_close_err = fclose(src);
    int dst_close_err = fclose(dst);
    if (src_close_err != 0 || dst_close_err != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && (s_card_removed || !card_present())) {
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        if (rename(temp_path, dst_path) != 0) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        remove(temp_path);
    }
    return err;
}

esp_err_t sd_storage_copy_to_littlefs(const char *path)
{
    if (!valid_relative_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted || !card_present()) {
        unlock_idle();
        return ESP_ERR_NOT_FOUND;
    }
    err = storage_fs_mount();
    if (err == ESP_OK) {
        char src[sizeof(SD_MOUNT_POINT) + SD_PATH_MAX];
        char dst[sizeof(STORAGE_FS_BASE) + SD_PATH_MAX];
        snprintf(src, sizeof(src), SD_MOUNT_POINT "/%s", path);
        snprintf(dst, sizeof(dst), STORAGE_FS_BASE "/%s", path);
        err = copy_file(src, dst);
    }
    unlock_idle();
    return err;
}

esp_err_t sd_storage_copy_from_littlefs(const char *path)
{
    if (!valid_relative_path(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted || !card_present()) {
        unlock_idle();
        return ESP_ERR_NOT_FOUND;
    }
    err = storage_fs_mount();
    if (err == ESP_OK) {
        char src[sizeof(STORAGE_FS_BASE) + SD_PATH_MAX];
        char dst[sizeof(SD_MOUNT_POINT) + SD_PATH_MAX];
        snprintf(src, sizeof(src), STORAGE_FS_BASE "/%s", path);
        snprintf(dst, sizeof(dst), SD_MOUNT_POINT "/%s", path);
        err = copy_file(src, dst);
    }
    unlock_idle();
    return err;
}

static void transfer_task(void *arg)
{
    (void)arg;
    FILE *file = fopen(s_transfer_path, "rb");
    if (!file) {
        io_log("sd: GET_ERROR id=%u err=open\r\n", (unsigned)s_transfer_id);
        goto done;
    }

    struct stat info;
    if (stat(s_transfer_path, &info) != 0 || info.st_size < 0) {
        fclose(file);
        io_log("sd: GET_ERROR id=%u err=stat\r\n", (unsigned)s_transfer_id);
        goto done;
    }
    size_t total_size = (size_t)info.st_size;
    size_t send_size = total_size < s_transfer_limit ? total_size : s_transfer_limit;
    io_log("sd: GET_BEGIN id=%u size=%u sent=%u\r\n",
           (unsigned)s_transfer_id, (unsigned)total_size, (unsigned)send_size);

    uint8_t data[SD_TRANSFER_CHUNK];
    size_t offset = 0;
    while (offset < send_size) {
        if (transfer_should_stop()) {
            io_log("sd: GET_ERROR id=%u err=cancelled-or-card-removed\r\n",
                   (unsigned)s_transfer_id);
            fclose(file);
            goto done;
        }
        size_t wanted = send_size - offset;
        if (wanted > sizeof(data)) {
            wanted = sizeof(data);
        }
        size_t count = fread(data, 1, wanted, file);
        if (count != wanted) {
            io_log("sd: GET_ERROR id=%u err=read\r\n",
                   (unsigned)s_transfer_id);
            fclose(file);
            goto done;
        }
        if (transfer_should_stop()) {
            io_log("sd: GET_ERROR id=%u err=cancelled-or-card-removed\r\n",
                   (unsigned)s_transfer_id);
            fclose(file);
            goto done;
        }

        char hex[SD_TRANSFER_CHUNK * 2 + 1];
        for (size_t i = 0; i < count; i++) {
            snprintf(hex + i * 2, 3, "%02x", data[i]);
        }
        hex[count * 2] = '\0';
        size_t next_offset = offset + count;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_transfer_ack_offset = next_offset;
        s_transfer_ack_pending = true;
        xSemaphoreGive(s_mutex);
        io_log("sd: DATA id=%u off=%u n=%u crc=%08x hex=%s\r\n",
               (unsigned)s_transfer_id, (unsigned)offset, (unsigned)count,
               (unsigned)transfer_crc32(data, count), hex);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000)) == 0) {
            io_log("sd: GET_ERROR id=%u err=ack-timeout\r\n",
                   (unsigned)s_transfer_id);
            fclose(file);
            goto done;
        }
        if (transfer_should_stop()) {
            io_log("sd: GET_ERROR id=%u err=cancelled-or-card-removed\r\n",
                   (unsigned)s_transfer_id);
            fclose(file);
            goto done;
        }
        offset = next_offset;
    }
    fclose(file);
    io_log("sd: GET_END id=%u size=%u sent=%u truncated=%u\r\n",
           (unsigned)s_transfer_id, (unsigned)total_size, (unsigned)send_size,
           send_size < total_size ? 1U : 0U);

done:
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_transfer_task = NULL;
    s_transfer_cancelled = false;
    s_transfer_ack_pending = false;
    s_busy = false;
    xSemaphoreGive(s_mutex);
    vTaskDelete(NULL);
}

esp_err_t sd_storage_download_start(const char *path, size_t max_bytes)
{
    if (!valid_relative_path(path) || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = lock_idle();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_mounted || !card_present()) {
        unlock_idle();
        return ESP_ERR_NOT_FOUND;
    }

    int len = snprintf(s_transfer_path, sizeof(s_transfer_path),
                       SD_MOUNT_POINT "/%s", path);
    if (len < 0 || (size_t)len >= sizeof(s_transfer_path)) {
        unlock_idle();
        return ESP_ERR_INVALID_ARG;
    }
    s_transfer_id++;
    if (s_transfer_id == 0) {
        s_transfer_id++;
    }
    s_transfer_limit = max_bytes;
    s_transfer_ack_offset = 0;
    s_transfer_ack_pending = false;
    s_transfer_cancelled = false;
    s_busy = true;
    if (xTaskCreate(transfer_task, "sd_download", 3072, NULL, 4,
                    &s_transfer_task) != pdPASS) {
        s_transfer_task = NULL;
        unlock_idle();
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_download_ack(uint32_t transfer_id, size_t next_offset)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_transfer_task || transfer_id != s_transfer_id ||
        !s_transfer_ack_pending ||
        next_offset != s_transfer_ack_offset) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    s_transfer_ack_pending = false;
    xTaskNotifyGive(s_transfer_task);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t sd_storage_download_cancel(uint32_t transfer_id)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_transfer_task || transfer_id != s_transfer_id) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_transfer_cancelled = true;
    xTaskNotifyGive(s_transfer_task);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void sd_storage_get_state(bool *present, bool *mounted, bool *busy,
                          bool *radio_safe)
{
    bool detected = s_initialized && card_present();
    if (present) {
        *present = detected;
    }
    if (mounted) {
        *mounted = s_mounted && detected && !s_card_removed;
    }
    if (busy) {
        *busy = s_busy;
    }
    if (radio_safe) {
        *radio_safe =
            s_radio_safe && spi_shared_bus_radio_access_allowed();
    }
}
