#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_loader.h"

typedef enum {
    ESP_FLASHER_STORAGE_LITTLEFS,
    ESP_FLASHER_STORAGE_SD,
} esp_flasher_storage_t;

typedef enum {
    ESP_FLASHER_RESULT_NONE,
    ESP_FLASHER_RESULT_RUNNING,
    ESP_FLASHER_RESULT_OK,
    ESP_FLASHER_RESULT_FAILED,
    ESP_FLASHER_RESULT_CANCELLED,
} esp_flasher_result_t;

#define ESP_FLASHER_MAX_IMAGES 3

esp_err_t esp_flasher_start_manual(
    esp_flasher_storage_t storage,
    const char *relative_path,
    uint32_t offset,
    target_chip_t expected_chip);
esp_err_t esp_flasher_batch_begin(
    esp_flasher_storage_t storage,
    uint8_t image_count,
    target_chip_t expected_chip);
esp_err_t esp_flasher_batch_add_image(
    uint8_t index,
    const char *relative_path,
    uint32_t offset);
esp_err_t esp_flasher_batch_start(void);
esp_err_t esp_flasher_batch_abort(void);
bool esp_flasher_is_busy(void);
void esp_flasher_get_operation_status(
    esp_flasher_result_t *result,
    uint32_t *programmed_bytes,
    uint32_t *image_bytes);
bool esp_flasher_cancel_is_requested(void);
esp_err_t esp_flasher_cancel(void);
bool esp_flasher_boot_is_asserted(void);
esp_err_t esp_flasher_reset_target(void);
esp_err_t esp_flasher_enter_bootloader(void);
esp_err_t esp_flasher_release_boot(void);
