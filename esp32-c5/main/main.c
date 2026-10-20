#include "nvs_flash.h"
#include "esp_err.h"
#include "io.h"
#include "cli.h"
#include "transport_serial.h"
#include "transport_ble.h"
#include "wifi_ctrl.h"
#include "targets.h"
#include "acl.h"
#include "attack.h"
#include "ble_spam.h"
#include "ble_scanner.h"
#include "ble_spectrum.h"
#include "ble_wardrive.h"
#include "nrf24.h"
#include "sd_storage.h"
#include "spi_shared_bus.h"
#include "monitor.h"
#include "spectrum.h"
#include "wifi_live_scan.h"
#include "wifi_spam.h"
#include "evil_portal.h"
#include "led.h"
#include "ir.h"
#include "temperature_monitor.h"
#include "sys_monitor.h"

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    io_init();

    esp_err_t spi_err = spi_shared_bus_init();
    bool spi_radios_safe = false;
    if (spi_err == ESP_OK) {
        io_log("main: shared SPI bus ready\r\n");
        esp_err_t sd_err = sd_storage_init();
        bool sd_present = false;
        bool sd_mounted = false;
        bool sd_busy = false;
        sd_storage_get_state(&sd_present, &sd_mounted, &sd_busy,
                             &spi_radios_safe);
        if (sd_err == ESP_OK) {
            io_log("main: SD card mounted before radio SPI access\r\n");
        } else if (!sd_present) {
            io_log("main: no SD card detected; shared SPI radio access enabled\r\n");
        } else {
            io_log("main: SD initialization failed (%s); radio SPI access blocked\r\n",
                   esp_err_to_name(sd_err));
        }
    } else {
        io_log("main: shared SPI initialization failed (%s)\r\n",
               esp_err_to_name(spi_err));
    }

    targets_init();
    acl_init();
    attack_init();
    wifi_ctrl_init();
    io_log("main: wifi up\r\n");
    cli_init();
    transport_serial_init();
    io_log("main: serial up\r\n");

    if (transport_ble_init()) {
        io_log("main: ble initialized; advertising pending NimBLE sync\r\n");
    } else {
        io_log("main: ble initialization failed; see ble diagnostics\r\n");
    }

    ble_spam_init();
    io_log("main: ble spam module ready\r\n");

    ble_scanner_init();
    io_log("main: ble scanner module ready\r\n");

    monitor_init();
    io_log("main: monitor module ready\r\n");

    spectrum_init();
    io_log("main: wifi spectrum module ready\r\n");

    ble_spectrum_init();
    io_log("main: ble spectrum module ready\r\n");

    ble_wardrive_init();
    io_log("main: ble wardrive module ready\r\n");

    if (spi_radios_safe && nrf24_module_init() == ESP_OK) {
        io_log("main: nrf24 module ready\r\n");
    } else {
        io_log("main: nrf24 not available\r\n");
    }

    esp_err_t wlscan_err = wifi_live_scan_init();
    if (wlscan_err == ESP_OK) {
        io_log("main: wifi live scan module ready\r\n");
    } else {
        io_log("main: wifi live scan module init failed: %s\r\n",
               esp_err_to_name(wlscan_err));
    }

    wifi_spam_init();
    io_log("main: wifi spam module ready\r\n");

    esp_err_t portal_err = evil_portal_init();
    if (portal_err == ESP_OK) {
        io_log("main: evil portal module ready\r\n");
    } else {
        io_log("main: evil portal init failed: %s\r\n",
               esp_err_to_name(portal_err));
    }

    if (ir_module_init() == ESP_OK) {
        io_log("main: ir module ready\r\n");
    } else {
        io_log("main: ir module init failed\r\n");
    }

    led_init();

    esp_err_t temperature_err = temperature_monitor_init();
    if (temperature_err == ESP_OK) {
        io_log("main: internal temperature monitor ready (4 Hz)\r\n");
    } else {
        io_log("main: internal temperature monitor failed: %s\r\n",
               esp_err_to_name(temperature_err));
    }

    esp_err_t sys_err = sys_monitor_init();
    if (sys_err == ESP_OK) {
        io_log("main: system telemetry heartbeat ready (1 Hz)\r\n");
    } else {
        io_log("main: system telemetry heartbeat failed: %s\r\n",
               esp_err_to_name(sys_err));
    }
}
