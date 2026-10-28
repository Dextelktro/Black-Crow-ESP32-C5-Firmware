#include "esp_flasher.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "board_config.h"
#include "io.h"
#include "esp_upload.h"
#include "sd_storage.h"
#include "storage_fs.h"

#define FLASHER_UART_PORT       UART_NUM_1
#define FLASHER_UART_TX_GPIO    BOARD_FLASHER_UART_TX_GPIO
#define FLASHER_UART_RX_GPIO    BOARD_FLASHER_UART_RX_GPIO
#define FLASHER_BOOT_GPIO       BOARD_FLASHER_BOOT_GPIO
#define FLASHER_RESET_GPIO      (-1)
#define FLASHER_UART_RX_BUFFER  2048
#define FLASHER_FLASH_BLOCK    1024
#define FLASHER_TASK_STACK     4096
#define FLASHER_PATH_MAX       192
#define FLASHER_CONNECT_TRIALS 25

typedef struct {
    esp_flasher_storage_t storage;
    target_chip_t expected_chip;
    uint32_t offset;
    char path[FLASHER_PATH_MAX];
} flash_job_t;

typedef struct {
    esp_flasher_storage_t storage;
    target_chip_t expected_chip;
    uint8_t image_count;
    flash_job_t images[ESP_FLASHER_MAX_IMAGES];
} flash_batch_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;
static bool s_manual_boot_active;   /* BARU: klaim exklusif BOOT pin manual, dilindungi s_state_lock sama seperti s_busy — mencegah esp_flasher_batch_start() dan esp_flasher_enter_bootloader() menyentuh FLASHER_BOOT_GPIO bersamaan */
static bool s_cancel_requested;
static esp_flasher_result_t s_flash_result;
static uint32_t s_flash_programmed;
static uint32_t s_flash_image_size;
static flash_batch_t s_job;
static flash_batch_t s_staged_batch;
static bool s_batch_staging;
static uint8_t s_staged_image_count;
static bool s_uart_installed;
static bool s_uart_pins_configured;
static bool s_boot_pin_initialized;
static bool s_boot_asserted;
static bool s_port_cleanup_failed;
static bool s_cleanup_mode;
static int64_t s_timer_deadline_us;

static const esp_loader_port_ops_t s_uart_ops;

static void flasher_log(const char *format, ...)
{
    char message[192];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    io_log("espflash: %s\r\n", message);
}

static bool cancel_requested(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool requested = s_cancel_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}

static void set_boot_asserted(bool asserted)
{
    portENTER_CRITICAL(&s_state_lock);
    s_boot_asserted = asserted;
    portEXIT_CRITICAL(&s_state_lock);
}

static void set_flash_progress(uint32_t programmed, uint32_t image_size)
{
    portENTER_CRITICAL(&s_state_lock);
    s_flash_programmed = programmed;
    s_flash_image_size = image_size;
    portEXIT_CRITICAL(&s_state_lock);
}

static void release_uart_pins(void)
{
    if (!s_uart_pins_configured) return;
    esp_err_t tx_err = gpio_reset_pin(FLASHER_UART_TX_GPIO);
    esp_err_t rx_err = gpio_reset_pin(FLASHER_UART_RX_GPIO);
    if (tx_err != ESP_OK || rx_err != ESP_OK) {
        s_port_cleanup_failed = true;
        flasher_log("UART GPIO release failed: TX=%s RX=%s",
                    esp_err_to_name(tx_err), esp_err_to_name(rx_err));
    }
    /* FIX: clear di luar if — sebelumnya cuma di-clear kalau SUKSES, jadi
     * sekali gpio_reset_pin gagal, s_uart_pins_configured nyangkut true
     * selamanya walau percobaan berikutnya sukses. gpio_reset_pin aman
     * dipanggil ulang, jadi tidak ada resource leak nyata; ini murni soal
     * state bookkeeping tetap akurat untuk diagnosa. */
    s_uart_pins_configured = false;
}

static bool valid_relative_path(const char *path)
{
    if (!path || path[0] == '\0' || path[0] == '/' ||
        strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static FILE *open_flash_file(const flash_job_t *job, char full_path[FLASHER_PATH_MAX])
{
    const char *prefix = job->storage == ESP_FLASHER_STORAGE_LITTLEFS
                             ? STORAGE_FS_BASE "/"
                             : "/sdcard/";
    if (job->storage == ESP_FLASHER_STORAGE_LITTLEFS) {
        if (storage_fs_mount() != ESP_OK) {
            flasher_log("LittleFS mount failed");
            return NULL;
        }
    }

    int n = snprintf(full_path, FLASHER_PATH_MAX, "%s%s", prefix, job->path);
    if (n < 0 || n >= FLASHER_PATH_MAX) {
        flasher_log("file path is too long");
        return NULL;
    }
    FILE *file = fopen(full_path, "rb");
    if (!file) flasher_log("cannot open %s", full_path);
    return file;
}

static esp_loader_error_t uart_port_init(esp_loader_port_t *port)
{
    (void)port;
    if (uart_is_driver_installed(FLASHER_UART_PORT)) {
        flasher_log("UART1 is already owned by another driver");
        return ESP_LOADER_ERROR_FAIL;
    }

    esp_err_t err = uart_driver_install(
        FLASHER_UART_PORT, FLASHER_UART_RX_BUFFER, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        flasher_log("UART driver install failed: %s", esp_err_to_name(err));
        return ESP_LOADER_ERROR_FAIL;
    }
    s_uart_installed = true;

    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(FLASHER_UART_PORT, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(
            FLASHER_UART_PORT,
            FLASHER_UART_TX_GPIO,
            FLASHER_UART_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
        if (err == ESP_OK) s_uart_pins_configured = true;
    }
    if (err == ESP_OK) {
        err = gpio_set_pull_mode(FLASHER_UART_RX_GPIO, GPIO_PULLUP_ONLY);
    }

    gpio_config_t boot_config = {
        .pin_bit_mask = 1ULL << FLASHER_BOOT_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (err == ESP_OK) err = gpio_config(&boot_config);
    if (err == ESP_OK) {
        s_boot_pin_initialized = true;
        err = gpio_set_level(FLASHER_BOOT_GPIO, 1);
        set_boot_asserted(false);
    }
    if (err != ESP_OK) {
        flasher_log("UART/GPIO setup failed: %s", esp_err_to_name(err));
        if (s_boot_pin_initialized) {
            esp_err_t boot_reset_err = gpio_reset_pin(FLASHER_BOOT_GPIO);
            if (boot_reset_err != ESP_OK) {
                s_port_cleanup_failed = true;
                flasher_log("BOOT GPIO cleanup failed: %s",
                            esp_err_to_name(boot_reset_err));
            }
            s_boot_pin_initialized = false;
            set_boot_asserted(false);
        }
        esp_err_t delete_err = uart_driver_delete(FLASHER_UART_PORT);
        if (delete_err == ESP_OK) {
            s_uart_installed = false;
            release_uart_pins();
        } else {
            s_port_cleanup_failed = true;
            flasher_log("UART cleanup failed: %s", esp_err_to_name(delete_err));
        }
        return ESP_LOADER_ERROR_FAIL;
    }

    err = uart_flush_input(FLASHER_UART_PORT);
    if (err != ESP_OK) {
        flasher_log("UART input flush failed: %s", esp_err_to_name(err));
        if (s_boot_pin_initialized) {
            esp_err_t level_err = gpio_set_level(FLASHER_BOOT_GPIO, 1);
            esp_err_t reset_err = gpio_reset_pin(FLASHER_BOOT_GPIO);
            if (level_err != ESP_OK || reset_err != ESP_OK) {
                s_port_cleanup_failed = true;
                flasher_log("BOOT GPIO cleanup failed after UART flush error: level=%s reset=%s",
                            esp_err_to_name(level_err), esp_err_to_name(reset_err));
            }
            s_boot_pin_initialized = false;
            set_boot_asserted(false);
        }
        esp_err_t delete_err = uart_driver_delete(FLASHER_UART_PORT);
        if (delete_err == ESP_OK) {
            s_uart_installed = false;
            release_uart_pins();
        } else {
            s_port_cleanup_failed = true;
            flasher_log("UART cleanup failed: %s", esp_err_to_name(delete_err));
        }
        return ESP_LOADER_ERROR_FAIL;
    }
    return ESP_LOADER_SUCCESS;
}

static void uart_port_deinit(esp_loader_port_t *port)
{
    (void)port;
    if (s_boot_pin_initialized) {
        esp_err_t level_err = gpio_set_level(FLASHER_BOOT_GPIO, 1);
        esp_err_t reset_err = gpio_reset_pin(FLASHER_BOOT_GPIO);
        if (level_err != ESP_OK || reset_err != ESP_OK) {
            s_port_cleanup_failed = true;
            flasher_log("BOOT GPIO release failed: level=%s reset=%s",
                        esp_err_to_name(level_err), esp_err_to_name(reset_err));
        }
        s_boot_pin_initialized = false;
        set_boot_asserted(false);
    }
    if (s_uart_installed) {
        esp_err_t err = uart_wait_tx_done(FLASHER_UART_PORT, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            s_port_cleanup_failed = true;
            flasher_log("UART TX drain failed: %s", esp_err_to_name(err));
        }
        err = uart_driver_delete(FLASHER_UART_PORT);
        if (err != ESP_OK) {
            s_port_cleanup_failed = true;
            flasher_log("UART driver delete failed: %s", esp_err_to_name(err));
        } else {
            s_uart_installed = false;
            release_uart_pins();
        }
    }
}

static void uart_port_enter_bootloader(esp_loader_port_t *port)
{
    (void)port;
    if (cancel_requested()) return;
    if (s_boot_pin_initialized) {
        if (gpio_set_level(FLASHER_BOOT_GPIO, 0) == ESP_OK) {
            set_boot_asserted(true);
        } else {
            flasher_log("could not assert BOOT during loader handshake");
        }
    }
}

static void uart_port_reset_target(esp_loader_port_t *port)
{
    (void)port;
    flasher_log("automatic RESET unavailable (configured pin is -1)");
}

static void uart_port_start_timer(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    s_timer_deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t uart_port_remaining_time(esp_loader_port_t *port)
{
    (void)port;
    int64_t remaining = s_timer_deadline_us - esp_timer_get_time();
    return remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0;
}

static void uart_port_delay_ms(esp_loader_port_t *port, uint32_t ms)
{
    (void)port;
    if (!cancel_requested()) vTaskDelay(pdMS_TO_TICKS(ms));
}

static void uart_port_log(
    esp_loader_port_t *port,
    esp_loader_log_level_t level,
    const char *format,
    va_list args)
{
    (void)port;
    (void)level;
    char message[192];
    vsnprintf(message, sizeof(message), format, args);
    flasher_log("%s", message);
}

static esp_loader_error_t uart_port_change_baud(
    esp_loader_port_t *port,
    uint32_t baud)
{
    (void)port;
    esp_err_t err = uart_wait_tx_done(FLASHER_UART_PORT, pdMS_TO_TICKS(100));
    if (err == ESP_OK) err = uart_set_baudrate(FLASHER_UART_PORT, baud);
    if (err != ESP_OK) {
        flasher_log("baud change to %lu failed: %s",
                    (unsigned long)baud, esp_err_to_name(err));
        return ESP_LOADER_ERROR_FAIL;
    }
    return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t uart_port_write(
    esp_loader_port_t *port,
    const uint8_t *data,
    uint16_t size,
    uint32_t timeout_ms)
{
    (void)port;
    if (cancel_requested() && !s_cleanup_mode) return ESP_LOADER_ERROR_FAIL;
    size_t written = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (written < size) {
        int count = uart_tx_chars(
            FLASHER_UART_PORT, (const char *)data + written, size - written);
        if (count < 0) return ESP_LOADER_ERROR_FAIL;
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (timeout_ms == 0 || esp_timer_get_time() >= deadline) {
            return ESP_LOADER_ERROR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t uart_port_read(
    esp_loader_port_t *port,
    uint8_t *data,
    uint16_t size,
    uint32_t timeout_ms)
{
    (void)port;
    if (cancel_requested() && !s_cleanup_mode) return ESP_LOADER_ERROR_FAIL;
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (received < size) {
        int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us <= 0) return ESP_LOADER_ERROR_TIMEOUT;
        uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        TickType_t wait = pdMS_TO_TICKS(remaining_ms);
        if (remaining_ms && wait == 0) wait = 1;
        int count = uart_read_bytes(
            FLASHER_UART_PORT, data + received, size - received, wait);
        if (count > 0) received += (size_t)count;
        else if (count < 0) return ESP_LOADER_ERROR_FAIL;
    }
    return ESP_LOADER_SUCCESS;
}

static const esp_loader_port_ops_t s_uart_ops = {
    .init = uart_port_init,
    .deinit = uart_port_deinit,
    .enter_bootloader = uart_port_enter_bootloader,
    .reset_target = uart_port_reset_target,
    .start_timer = uart_port_start_timer,
    .remaining_time = uart_port_remaining_time,
    .delay_ms = uart_port_delay_ms,
    .log = uart_port_log,
    .log_hex = NULL,
    .change_transmission_rate = uart_port_change_baud,
    .write = uart_port_write,
    .read = uart_port_read,
    .spi_set_cs = NULL,
    .sdio_write = NULL,
    .sdio_read = NULL,
    .sdio_card_init = NULL,
};

static const char *chip_name(target_chip_t chip)
{
    switch (chip) {
    case ESP8266_CHIP: return "ESP8266";
    case ESP32_CHIP: return "ESP32";
    case ESP32S2_CHIP: return "ESP32-S2";
    case ESP32C2_CHIP: return "ESP32-C2";
    case ESP32C3_CHIP: return "ESP32-C3";
    case ESP32S3_CHIP: return "ESP32-S3";
    case ESP32C5_CHIP: return "ESP32-C5";
    case ESP32H2_CHIP: return "ESP32-H2";
    case ESP32C6_CHIP: return "ESP32-C6";
    case ESP32P4_CHIP: return "ESP32-P4";
    case ESP32C61_CHIP: return "ESP32-C61";
    case ESP32S31_CHIP: return "ESP32-S31";
    case ESP32H21_CHIP: return "ESP32-H21";
    case ESP32H4_CHIP: return "ESP32-H4";
    default: return "unsupported";
    }
}

static esp_err_t flash_batch(esp_loader_t *loader, const flash_batch_t *batch)
{
    esp_err_t result = ESP_FAIL;
    FILE *files[ESP_FLASHER_MAX_IMAGES] = {0};
    char full_path[FLASHER_PATH_MAX];
    uint32_t file_sizes[ESP_FLASHER_MAX_IMAGES] = {0};
    uint32_t padded_sizes[ESP_FLASHER_MAX_IMAGES] = {0};
    uint32_t total_size = 0;
    bool sd_reserved = false;
    bool loader_initialized = false;
    bool flash_started = false;
    uint8_t *payload = NULL;
    uint8_t current_image = 0;
    esp_loader_flash_cfg_t cfg = {0};
    esp_loader_port_t port = {.ops = &s_uart_ops};
    memset(loader, 0, sizeof(*loader));

    if (batch->image_count == 0 ||
        batch->image_count > ESP_FLASHER_MAX_IMAGES) {
        flasher_log("invalid batch image count");
        goto cleanup;
    }
    if (batch->storage == ESP_FLASHER_STORAGE_SD) {
        esp_err_t reserve_err = sd_storage_acquire();
        if (reserve_err != ESP_OK) {
            flasher_log("SD must be mounted and idle (%s)",
                        esp_err_to_name(reserve_err));
            goto cleanup;
        }
        sd_reserved = true;
    }

    for (uint8_t i = 0; i < batch->image_count; i++) {
        const flash_job_t *job = &batch->images[i];
        files[i] = open_flash_file(job, full_path);
        if (!files[i]) goto cleanup;
        if (setvbuf(files[i], NULL, _IONBF, 0) != 0 ||
            fseek(files[i], 0, SEEK_END) != 0) {
            flasher_log("cannot inspect source file %s", full_path);
            goto cleanup;
        }
        long file_length = ftell(files[i]);
        if (file_length <= 0 || (uint64_t)file_length > UINT32_MAX - 3U) {
            flasher_log("invalid file length for %s", full_path);
            goto cleanup;
        }
        file_sizes[i] = (uint32_t)file_length;
        padded_sizes[i] = (file_sizes[i] + 3U) & ~3U;
        if (UINT32_MAX - total_size < file_sizes[i]) {
            flasher_log("combined image size exceeds supported progress range");
            goto cleanup;
        }
        total_size += file_sizes[i];
        if (fseek(files[i], 0, SEEK_SET) != 0) {
            flasher_log("cannot rewind %s", full_path);
            goto cleanup;
        }
    }
    set_flash_progress(0, total_size);

    payload = heap_caps_malloc(
        FLASHER_FLASH_BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
        flasher_log("PSRAM flash buffer allocation failed");
        goto cleanup;
    }

    esp_loader_error_t err = esp_loader_init_serial(loader, &port);
    if (err != ESP_LOADER_SUCCESS) {
        flasher_log("loader/UART initialization failed (%d)", (int)err);
        uart_port_deinit(&port);
        goto cleanup;
    }
    loader_initialized = true;

    flasher_log("DETECTING; hold target BOOT and manually reset/power-cycle it now");
    esp_loader_connect_args_t connect = ESP_LOADER_CONNECT_DEFAULT();
    connect.trials = FLASHER_CONNECT_TRIALS;
    err = esp_loader_connect(loader, &connect);
    if (err != ESP_LOADER_SUCCESS) {
        flasher_log("connection failed (%d); no flash write was started", (int)err);
        goto cleanup;
    }

    target_chip_t chip = esp_loader_get_target(loader);
    flasher_log("connected to %s", chip_name(chip));
    if (chip == ESP_UNKNOWN_CHIP ||
        (batch->expected_chip != ESP_UNKNOWN_CHIP &&
         chip != batch->expected_chip)) {
        flasher_log("target/profile mismatch; no flash write was started");
        goto cleanup;
    }

    uint32_t flash_size = 0;
    err = esp_loader_flash_detect_size(loader, &flash_size);
    if (err != ESP_LOADER_SUCCESS || flash_size == 0) {
        flasher_log("target flash size could not be verified; refusing to write");
        goto cleanup;
    }

    for (uint8_t i = 0; i < batch->image_count; i++) {
        const flash_job_t *job = &batch->images[i];
        uint64_t end = (uint64_t)job->offset + padded_sizes[i];
        if ((job->offset & 3U) != 0 || end > flash_size) {
            flasher_log("image %u range 0x%08lx..0x%08llx exceeds target flash",
                        (unsigned)(i + 1), (unsigned long)job->offset,
                        (unsigned long long)end);
            goto cleanup;
        }
        for (uint8_t j = 0; j < i; j++) {
            uint64_t other_end =
                (uint64_t)batch->images[j].offset + padded_sizes[j];
            if ((uint64_t)job->offset < other_end &&
                (uint64_t)batch->images[j].offset < end) {
                flasher_log("image %u overlaps image %u; refusing batch",
                            (unsigned)(i + 1), (unsigned)(j + 1));
                goto cleanup;
            }
        }
    }

    uint32_t completed_size = 0;
    for (uint8_t i = 0; i < batch->image_count; i++) {
        const flash_job_t *job = &batch->images[i];
        if (cancel_requested()) {
            flasher_log("cancelled before image %u/%u",
                        (unsigned)(i + 1), (unsigned)batch->image_count);
            goto cleanup;
        }
        current_image = (uint8_t)(i + 1);
        flasher_log("image %u/%u start path=%s offset=0x%08lx size=%lu",
                    (unsigned)current_image, (unsigned)batch->image_count,
                    job->path, (unsigned long)job->offset,
                    (unsigned long)file_sizes[i]);
        cfg = (esp_loader_flash_cfg_t){
            .offset = job->offset,
            .image_size = padded_sizes[i],
            .block_size = FLASHER_FLASH_BLOCK,
            .skip_verify = chip == ESP8266_CHIP,
        };
        err = esp_loader_flash_start(loader, &cfg);
        if (err != ESP_LOADER_SUCCESS) {
            flasher_log("image %u/%u erase/start failed (%d)",
                        (unsigned)current_image, (unsigned)batch->image_count,
                        (int)err);
            goto cleanup;
        }
        flash_started = true;

        uint32_t file_remaining = file_sizes[i];
        uint32_t image_remaining = padded_sizes[i];
        uint32_t written = 0;
        uint32_t last_reported = 0;
        bool write_ok = true;
        while (image_remaining > 0) {
            if (cancel_requested()) {
                flasher_log("cancel requested during image %u/%u",
                            (unsigned)current_image,
                            (unsigned)batch->image_count);
                write_ok = false;
                break;
            }
            size_t read_size = file_remaining < FLASHER_FLASH_BLOCK
                                   ? file_remaining
                                   : FLASHER_FLASH_BLOCK;
            size_t block_size = image_remaining < FLASHER_FLASH_BLOCK
                                    ? image_remaining
                                    : FLASHER_FLASH_BLOCK;
            if (read_size > 0 &&
                fread(payload, 1, read_size, files[i]) != read_size) {
                flasher_log("image %u/%u short read at byte %lu",
                            (unsigned)current_image,
                            (unsigned)batch->image_count,
                            (unsigned long)written);
                write_ok = false;
                break;
            }
            if (block_size > read_size) {
                memset(payload + read_size, 0xFF, block_size - read_size);
            }
            err = esp_loader_flash_write(loader, &cfg, payload, block_size);
            if (err != ESP_LOADER_SUCCESS) {
                flasher_log("image %u/%u write failed at byte %lu (%d)",
                            (unsigned)current_image,
                            (unsigned)batch->image_count,
                            (unsigned long)written, (int)err);
                write_ok = false;
                break;
            }
            file_remaining -= (uint32_t)read_size;
            image_remaining -= (uint32_t)block_size;
            written += (uint32_t)read_size;
            set_flash_progress(completed_size + written, total_size);
            if (written == file_sizes[i] ||
                written - last_reported >= 16U * 1024U) {
                flasher_log("programmed %lu/%lu bytes (image %u/%u)",
                            (unsigned long)(completed_size + written),
                            (unsigned long)total_size,
                            (unsigned)current_image,
                            (unsigned)batch->image_count);
                last_reported = written;
            }
        }

        if (!write_ok) {
            cfg.skip_verify = true;
            s_cleanup_mode = true;
            esp_loader_error_t finish_err = esp_loader_flash_finish(loader, &cfg);
            s_cleanup_mode = false;
            flash_started = false;
            if (finish_err != ESP_LOADER_SUCCESS) {
                flasher_log("image %u cleanup failed (%d)",
                            (unsigned)current_image, (int)finish_err);
            }
            goto cleanup;
        }

        err = esp_loader_flash_finish(loader, &cfg);
        flash_started = false;
        if (err != ESP_LOADER_SUCCESS) {
            flasher_log("image %u/%u finish/verification failed (%d)",
                        (unsigned)current_image, (unsigned)batch->image_count,
                        (int)err);
            goto cleanup;
        }
        completed_size += file_sizes[i];
        set_flash_progress(completed_size, total_size);
        if (cfg.skip_verify) {
            flasher_log("image %u/%u written; ROM MD5 verification unavailable on ESP8266",
                        (unsigned)current_image, (unsigned)batch->image_count);
        } else {
            flasher_log("image %u/%u write and target MD5 verification succeeded",
                        (unsigned)current_image, (unsigned)batch->image_count);
        }
    }
    result = ESP_OK;

cleanup:
    if (flash_started) {
        cfg.skip_verify = true;
        s_cleanup_mode = true;
        esp_loader_error_t finish_err = esp_loader_flash_finish(loader, &cfg);
        s_cleanup_mode = false;
        if (finish_err != ESP_LOADER_SUCCESS) {
            flasher_log("flash-session cleanup failed (%d)", (int)finish_err);
        }
    }
    if (result != ESP_OK && current_image > 0) {
        flasher_log("batch stopped at image %u/%u; current image may be partial and earlier images remain written; no rollback",
                    (unsigned)current_image, (unsigned)batch->image_count);
    }
    if (loader_initialized) esp_loader_deinit(loader);
    if (s_port_cleanup_failed) {
        flasher_log("UART/BOOT resources were not fully released");
        result = ESP_FAIL;
    }
    if (payload) heap_caps_free(payload);
    for (uint8_t i = 0; i < ESP_FLASHER_MAX_IMAGES; i++) {
        if (files[i] && fclose(files[i]) != 0) {
            flasher_log("source file close failed for image %u",
                        (unsigned)(i + 1));
            result = ESP_FAIL;
        }
    }
    if (sd_reserved) {
        esp_err_t release_err = sd_storage_release();
        if (release_err != ESP_OK) {
            flasher_log("SD release failed: %s", esp_err_to_name(release_err));
            result = ESP_FAIL;
        }
    }
    return result;
}

static void flash_worker(void *arg)
{
    (void)arg;
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_loader_t loader;
    s_port_cleanup_failed = false;
    s_cleanup_mode = false;
    esp_err_t result = flash_batch(&loader, &s_job);
    flasher_log("DONE result=%s", result == ESP_OK ? "ok" : "failed");
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    flasher_log("session heap delta: internal %ld, PSRAM %ld; stack high-water %u",
                (long)internal_after - (long)internal_before,
                (long)psram_after - (long)psram_before,
                (unsigned)uxTaskGetStackHighWaterMark(NULL));

    portENTER_CRITICAL(&s_state_lock);
    s_flash_result = result == ESP_OK
        ? ESP_FLASHER_RESULT_OK
        : s_cancel_requested ? ESP_FLASHER_RESULT_CANCELLED
        : ESP_FLASHER_RESULT_FAILED;
    memset(&s_job, 0, sizeof(s_job));
    s_busy = false;
    s_cancel_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
    vTaskDelete(NULL);
}

esp_err_t esp_flasher_batch_begin(
    esp_flasher_storage_t storage,
    uint8_t image_count,
    target_chip_t expected_chip)
{
    if ((storage != ESP_FLASHER_STORAGE_LITTLEFS &&
         storage != ESP_FLASHER_STORAGE_SD) ||
        image_count == 0 || image_count > ESP_FLASHER_MAX_IMAGES) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_busy || esp_upload_is_busy()) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_staged_batch, 0, sizeof(s_staged_batch));
    s_staged_batch.storage = storage;
    s_staged_batch.expected_chip = expected_chip;
    s_staged_batch.image_count = image_count;
    s_staged_image_count = 0;
    s_batch_staging = true;
    portEXIT_CRITICAL(&s_state_lock);

    flasher_log("batch configured for %u image(s)",
                (unsigned)image_count);
    return ESP_OK;
}

esp_err_t esp_flasher_batch_add_image(
    uint8_t index,
    const char *relative_path,
    uint32_t offset)
{
    if (!valid_relative_path(relative_path) ||
        strlen(relative_path) >= sizeof(s_staged_batch.images[0].path) ||
        (offset & 3U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (s_busy || !s_batch_staging ||
        index != s_staged_image_count ||
        index >= s_staged_batch.image_count) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    flash_job_t *image = &s_staged_batch.images[index];
    image->storage = s_staged_batch.storage;
    image->expected_chip = s_staged_batch.expected_chip;
    image->offset = offset;
    strlcpy(image->path, relative_path, sizeof(image->path));
    s_staged_image_count++;
    portEXIT_CRITICAL(&s_state_lock);

    flasher_log("batch image %u configured path=%s offset=0x%08lx",
                (unsigned)(index + 1), relative_path, (unsigned long)offset);
    return ESP_OK;
}

esp_err_t esp_flasher_batch_start(void)
{
#if !BOARD_HAS_ESP_FLASHER
    flasher_log("ESP-flasher is not supported on this board (no spare UART pins)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    portENTER_CRITICAL(&s_state_lock);
    if (s_busy || !s_batch_staging || s_manual_boot_active ||   /* FIX: tolak kalau BOOT sedang dipegang manual */
        s_staged_image_count != s_staged_batch.image_count ||
        esp_upload_is_busy()) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t image_count = s_staged_batch.image_count;
    s_job = s_staged_batch;
    memset(&s_staged_batch, 0, sizeof(s_staged_batch));
    s_batch_staging = false;
    s_staged_image_count = 0;
    s_busy = true;
    s_cancel_requested = false;
    s_flash_result = ESP_FLASHER_RESULT_RUNNING;
    s_flash_programmed = 0;
    s_flash_image_size = 0;
    portEXIT_CRITICAL(&s_state_lock);

    if (xTaskCreate(flash_worker, "esp_flasher", FLASHER_TASK_STACK,
                    NULL, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_busy = false;
        s_cancel_requested = false;
        s_flash_result = ESP_FLASHER_RESULT_FAILED;
        memset(&s_job, 0, sizeof(s_job));
        portEXIT_CRITICAL(&s_state_lock);
        flasher_log("could not create flash task");
        return ESP_ERR_NO_MEM;
    }

    flasher_log("flash batch started with %u image(s); target detection runs once",
                (unsigned)image_count);
    return ESP_OK;
}

esp_err_t esp_flasher_batch_abort(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_busy || !s_batch_staging) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_staged_batch, 0, sizeof(s_staged_batch));
    s_staged_image_count = 0;
    s_batch_staging = false;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t esp_flasher_start_manual(
    esp_flasher_storage_t storage,
    const char *relative_path,
    uint32_t offset,
    target_chip_t expected_chip)
{
    esp_err_t err = esp_flasher_batch_begin(storage, 1, expected_chip);
    if (err != ESP_OK) return err;
    err = esp_flasher_batch_add_image(0, relative_path, offset);
    if (err != ESP_OK) {
        (void)esp_flasher_batch_abort();
        return err;
    }
    return esp_flasher_batch_start();
}

bool esp_flasher_is_busy(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool busy = s_busy;
    portEXIT_CRITICAL(&s_state_lock);
    return busy;
}

void esp_flasher_get_operation_status(
    esp_flasher_result_t *result,
    uint32_t *programmed_bytes,
    uint32_t *image_bytes)
{
    portENTER_CRITICAL(&s_state_lock);
    if (result) *result = s_flash_result;
    if (programmed_bytes) *programmed_bytes = s_flash_programmed;
    if (image_bytes) *image_bytes = s_flash_image_size;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t esp_flasher_cancel(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_busy) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_cancel_requested = true;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

bool esp_flasher_cancel_is_requested(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool requested = s_cancel_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}

bool esp_flasher_boot_is_asserted(void)
{
    portENTER_CRITICAL(&s_state_lock);
    bool asserted = s_boot_asserted;
    portEXIT_CRITICAL(&s_state_lock);
    return asserted;
}

esp_err_t esp_flasher_reset_target(void)
{
    if (FLASHER_RESET_GPIO < 0) {
        flasher_log("automatic reset unavailable; RESET pin is not configured");
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_flasher_enter_bootloader(void)
{
#if !BOARD_HAS_ESP_FLASHER
    flasher_log("ESP-flasher is not supported on this board (no spare UART pins)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    /* FIX: klaim s_manual_boot_active DI DALAM lock, bersamaan dengan cek
     * s_busy/esp_upload_is_busy() — menutup celah race yang sebelumnya ada
     * antara "cek tidak busy" dan "mulai sentuh GPIO" (lihat pembahasan
     * sebelumnya). esp_upload_is_busy() aman dipanggil di sini karena dia
     * tidak mengambil lock apa pun di dalamnya (cuma baca bool), sama
     * seperti yang sudah dilakukan esp_flasher_batch_begin()/batch_start(). */
    portENTER_CRITICAL(&s_state_lock);
    if (s_busy || s_manual_boot_active || esp_upload_is_busy()) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_manual_boot_active = true;
    portEXIT_CRITICAL(&s_state_lock);

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << FLASHER_BOOT_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) s_boot_pin_initialized = true;
    if (err == ESP_OK) {
        err = gpio_set_level(FLASHER_BOOT_GPIO, 0);
        if (err == ESP_OK) set_boot_asserted(true);
    }
    if (err == ESP_OK) {
        flasher_log("BOOT is asserted; manually reset/power-cycle target, then release BOOT");
    } else {
        if (s_boot_pin_initialized) {
            esp_err_t level_err = gpio_set_level(FLASHER_BOOT_GPIO, 1);
            esp_err_t reset_err = gpio_reset_pin(FLASHER_BOOT_GPIO);
            if (level_err != ESP_OK || reset_err != ESP_OK) {
                s_port_cleanup_failed = true;
                flasher_log("BOOT GPIO setup rollback failed: level=%s reset=%s",
                            esp_err_to_name(level_err), esp_err_to_name(reset_err));
            }
            s_boot_pin_initialized = false;
            set_boot_asserted(false);
        }
        flasher_log("could not assert BOOT: %s", esp_err_to_name(err));
        /* FIX: gagal total — lepas klaim supaya bisa dicoba lagi, bukan macet
         * permanen menganggap BOOT masih "dipegang" padahal gagal di-assert. */
        portENTER_CRITICAL(&s_state_lock);
        s_manual_boot_active = false;
        portEXIT_CRITICAL(&s_state_lock);
    }
    return err;
}

esp_err_t esp_flasher_release_boot(void)
{
    if (esp_flasher_is_busy() || esp_upload_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_boot_pin_initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t err = gpio_set_level(FLASHER_BOOT_GPIO, 1);
    if (err != ESP_OK) s_port_cleanup_failed = true;
    if (err == ESP_OK) {
        err = gpio_reset_pin(FLASHER_BOOT_GPIO);
        if (err == ESP_OK) {
            s_boot_pin_initialized = false;
            set_boot_asserted(false);
        } else {
            s_port_cleanup_failed = true;
        }
    }
    if (err != ESP_OK) {
        flasher_log("could not release BOOT: %s", esp_err_to_name(err));
    }
    /* FIX: lepas klaim di jalur manapun (sukses atau gagal) — supaya
     * esp_flasher_batch_start() bisa jalan lagi setelah ini, dan supaya
     * esp_flasher_enter_bootloader() berikutnya tidak ditolak permanen. */
    portENTER_CRITICAL(&s_state_lock);
    s_manual_boot_active = false;
    portEXIT_CRITICAL(&s_state_lock);
    return err;
}
