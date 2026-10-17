#include "temperature_monitor.h"

#include <stdbool.h>

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "io.h"

#define TEMPERATURE_SAMPLE_PERIOD_MS 250

static const char *TAG = "temperature_monitor";
static temperature_sensor_handle_t s_sensor;

static void cleanup_sensor(bool enabled)
{
    if (enabled) {
        esp_err_t err = temperature_sensor_disable(s_sensor);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "temperature sensor disable failed: %s",
                     esp_err_to_name(err));
        }
    }

    esp_err_t err = temperature_sensor_uninstall(s_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature sensor uninstall failed: %s",
                 esp_err_to_name(err));
        return;
    }
    s_sensor = NULL;
}

static void temperature_monitor_task(void *arg)
{
    (void)arg;
    TickType_t wake_time = xTaskGetTickCount();

    for (;;) {
        float temperature_c;
        esp_err_t err = temperature_sensor_get_celsius(s_sensor, &temperature_c);
        if (err == ESP_OK) {
            io_log("temperature: %.1f C\r\n", (double)temperature_c);
        } else {
            ESP_LOGW(TAG, "temperature sample failed: %s", esp_err_to_name(err));
        }

        vTaskDelayUntil(&wake_time, pdMS_TO_TICKS(TEMPERATURE_SAMPLE_PERIOD_MS));
    }
}

esp_err_t temperature_monitor_init(void)
{
    const temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&config, &s_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature sensor install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = temperature_sensor_enable(s_sensor);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temperature sensor enable failed: %s", esp_err_to_name(err));
        cleanup_sensor(false);
        return err;
    }

    if (xTaskCreate(temperature_monitor_task, "temperature_monitor", 4096,
                    NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "temperature monitor task creation failed");
        cleanup_sensor(true);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
