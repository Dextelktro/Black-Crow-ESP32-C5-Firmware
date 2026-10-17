#include "nrf24.h"

#include "esp_log.h"
#include "io.h"

static const char *TAG = "nrf24_mod";
static bool s_ready = false;

esp_err_t nrf24_module_init(void)
{
    nrf24_pins_t pins = {
        .pin_sck      = 6,
        .pin_miso     = 2,
        .pin_mosi     = 7,
        .pin_csn      = 9,
        .pin_ce       = 8,
        .pin_irq      = -1,
        .spi_clock_hz = 4000000,
    };

    esp_err_t err = nrf24_init(&pins);
    if (err != ESP_OK) {
        io_log("nrf24: driver init failed (%d)\r\n", err);
        return err;
    }

    if (!nrf24_is_present()) {
        io_log("nrf24: module not detected\r\n");
        return ESP_ERR_NOT_FOUND;
    }

    nrf24_scanner_init();
    nrf24_jammer_init();
    err = nrf24_sniffer_init();
    if (err != ESP_OK) {
        io_log("nrf24: sniffer init failed (%d)\r\n", err);
        return err;
    }
    nrf24_mousejack_init();

    nrf24_dump_status();
    io_log("nrf24: module ready\r\n");
    s_ready = true;
    return ESP_OK;
}

bool nrf24_module_ready(void) { return s_ready; }