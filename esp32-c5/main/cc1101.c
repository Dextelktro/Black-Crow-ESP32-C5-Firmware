#include "cc1101.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

#include "spi_shared_bus.h"

#define CC1101_CS GPIO_NUM_23
#define CC1101_GDO0 GPIO_NUM_24
#define CC1101_SPI_CLOCK_HZ 2000000
#define CC1101_READY_TIMEOUT_US 1000
#define CC1101_VERSION 0x31
#define CC1101_READ 0x80
#define CC1101_BURST 0x40
#define CC1101_SIDLE 0x36
#define CC1101_SPWD 0x39

static spi_device_handle_t s_spi;
static bool s_present;
static bool s_sleeping;

static esp_err_t select_device(void)
{
    if (!spi_shared_bus_radio_access_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    if (!spi_shared_bus_radio_access_allowed()) {
        spi_device_release_bus(s_spi);
        return ESP_ERR_INVALID_STATE;
    }

    gpio_set_level(CC1101_CS, 0);
    for (uint32_t waited = 0; waited < CC1101_READY_TIMEOUT_US; waited++) {
        if (gpio_get_level(GPIO_NUM_2) == 0) {
            return ESP_OK;
        }
        esp_rom_delay_us(1);
    }

    gpio_set_level(CC1101_CS, 1);
    spi_device_release_bus(s_spi);
    return ESP_ERR_TIMEOUT;
}

static void deselect_device(void)
{
    gpio_set_level(CC1101_CS, 1);
    spi_device_release_bus(s_spi);
}

static esp_err_t transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_spi || !tx || len == 0 || len > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = select_device();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t rx_buf[2] = {0};
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx_buf,
    };
    err = spi_device_polling_transmit(s_spi, &transaction);
    deselect_device();
    if (err == ESP_OK && rx) {
        memcpy(rx, rx_buf, len);
    }
    return err;
}

static esp_err_t read_version(uint8_t *version)
{
    const uint8_t tx[2] = {CC1101_VERSION | CC1101_READ | CC1101_BURST, 0xFF};
    uint8_t rx[2] = {0};
    esp_err_t err = transfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *version = rx[1];
    }
    return err;
}

static esp_err_t strobe(uint8_t command)
{
    return transfer(&command, NULL, sizeof(command));
}

esp_err_t cc1101_init(void)
{
    if (s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!spi_shared_bus_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t gdo_config = {
        .pin_bit_mask = 1ULL << CC1101_GDO0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gdo_config);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = CC1101_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(spi_shared_bus_host(), &device_config, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t version = 0;
    err = read_version(&version);
    if (err != ESP_OK || version == 0x00 || version == 0xFF) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    s_present = true;
    err = cc1101_sleep();
    if (err != ESP_OK) {
        s_present = false;
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t cc1101_sleep(void)
{
    if (!s_spi || !s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sleeping) {
        return ESP_OK;
    }

    esp_err_t err = strobe(CC1101_SIDLE);
    if (err == ESP_OK) {
        err = strobe(CC1101_SPWD);
    }
    if (err == ESP_OK) {
        s_sleeping = true;
    }
    return err;
}

esp_err_t cc1101_wake(void)
{
    if (!s_spi || !s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sleeping) {
        return ESP_OK;
    }

    esp_err_t err = select_device();
    if (err == ESP_OK) {
        deselect_device();
        s_sleeping = false;
    }
    return err;
}

esp_err_t cc1101_deinit(void)
{
    if (!s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_present ? cc1101_sleep() : ESP_OK;
    esp_err_t remove_err = spi_bus_remove_device(s_spi);
    s_spi = NULL;
    s_present = false;
    s_sleeping = false;
    return err != ESP_OK ? err : remove_err;
}

bool cc1101_is_present(void)
{
    return s_present;
}

bool cc1101_is_sleeping(void)
{
    return s_sleeping;
}
