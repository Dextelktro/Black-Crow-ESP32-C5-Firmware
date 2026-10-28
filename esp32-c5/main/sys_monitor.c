/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sys_monitor.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io.h"
#include "storage_fs.h"

#define SYS_REPORT_PERIOD_MS 250
#define SYS_CPU_WINDOW_COUNT 4

static const char *TAG = "sys_monitor";

static void sys_monitor_task(void *arg)
{
    (void)arg;
    uint32_t idle_windows_us[SYS_CPU_WINDOW_COUNT] = {0};
    uint64_t elapsed_windows_us[SYS_CPU_WINDOW_COUNT] = {0};
    uint32_t window_index = 0;
    uint32_t window_count = 0;
    int64_t last_report_us = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE last_idle_us =
        ulTaskGetIdleRunTimeCounter();

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const uint64_t elapsed_us = (uint64_t)(now_us - last_report_us);
        if (elapsed_us >= SYS_REPORT_PERIOD_MS * 1000U) {
            const configRUN_TIME_COUNTER_TYPE idle_total_us =
                ulTaskGetIdleRunTimeCounter();
            uint32_t idle_elapsed_us = idle_total_us - last_idle_us;
            if (idle_elapsed_us > elapsed_us) {
                idle_elapsed_us = (uint32_t)elapsed_us;
            }

            idle_windows_us[window_index] = idle_elapsed_us;
            elapsed_windows_us[window_index] = elapsed_us;
            window_index = (window_index + 1U) % SYS_CPU_WINDOW_COUNT;
            if (window_count < SYS_CPU_WINDOW_COUNT) {
                window_count++;
            }

            uint64_t measured_idle_us = 0;
            uint64_t measured_elapsed_us = 0;
            for (uint32_t i = 0; i < window_count; i++) {
                measured_idle_us += idle_windows_us[i];
                measured_elapsed_us += elapsed_windows_us[i];
            }
            const uint32_t cpu_percent = measured_elapsed_us
                ? (uint32_t)(((measured_elapsed_us - measured_idle_us) * 100U
                              + measured_elapsed_us / 2U)
                             / measured_elapsed_us)
                : 0;

            const size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            const size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
            const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

            size_t fs_total = 0, fs_used = 0;
            if (storage_fs_info(&fs_total, &fs_used) != ESP_OK) {
                fs_total = 0;
                fs_used = 0;
            }
            const size_t fs_free = (fs_total >= fs_used) ? (fs_total - fs_used) : 0;

            io_log("sys: cpu=%u heap=%u/%u psram=%u/%u flash=%u/%u\r\n",
                   (unsigned)cpu_percent,
                   (unsigned)heap_free, (unsigned)heap_total,
                   (unsigned)psram_free, (unsigned)psram_total,
                   (unsigned)fs_free, (unsigned)fs_total);

            last_report_us = now_us;
            last_idle_us = idle_total_us;
        }

        vTaskDelay(pdMS_TO_TICKS(SYS_REPORT_PERIOD_MS));
    }
}

esp_err_t sys_monitor_init(void)
{
    /* Ensure the LittleFS partition is mounted so flash telemetry is valid even
     * if no feature that owns it (IR, Evil Portal) has started yet. Idempotent;
     * a failure here just means flash reports 0/0 until a feature mounts it. */
    storage_fs_mount();

    if (xTaskCreate(sys_monitor_task, "sys_monitor", 4096, NULL, 1, NULL)
            != pdPASS) {
        ESP_LOGE(TAG, "sys monitor task creation failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
