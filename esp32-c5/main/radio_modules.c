#include "radio_modules.h"

#include <stdbool.h>

#include "cc1101.h"
#include "io.h"
#include "nrf24.h"
#include "spi_shared_bus.h"

static bool s_nrf24_attempted;
static bool s_cc1101_attempted;
static esp_err_t s_nrf24_result = ESP_OK;
static esp_err_t s_cc1101_result = ESP_OK;

esp_err_t radio_modules_init(void)
{
    if (!spi_shared_bus_is_ready() ||
        !spi_shared_bus_radio_access_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_nrf24_attempted) {
        s_nrf24_attempted = true;
        s_nrf24_result = nrf24_module_init();
        if (s_nrf24_result == ESP_OK) {
            io_log("main: nrf24 module ready (powered down while idle)\r\n");
        } else {
            io_log("main: nrf24 not available (%s)\r\n",
                   esp_err_to_name(s_nrf24_result));
        }
    }

    if (!s_cc1101_attempted) {
        s_cc1101_attempted = true;
        s_cc1101_result = cc1101_init();
        if (s_cc1101_result == ESP_OK) {
            io_log("main: cc1101 module ready and sleeping\r\n");
        } else {
            io_log("main: cc1101 not available (%s)\r\n",
                   esp_err_to_name(s_cc1101_result));
        }
    }

    return s_nrf24_result != ESP_OK ? s_nrf24_result : s_cc1101_result;
}
