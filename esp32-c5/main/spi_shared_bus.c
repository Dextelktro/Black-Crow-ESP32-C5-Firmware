/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spi_shared_bus.h"

#include "driver/gpio.h"

#include "board_config.h"

#define SHARED_SPI_HOST SPI2_HOST
#define SHARED_SPI_SCK BOARD_SPI_SCK_GPIO
#define SHARED_SPI_MISO BOARD_SPI_MISO_GPIO
#define SHARED_SPI_MOSI BOARD_SPI_MOSI_GPIO

static bool s_ready;
static volatile bool s_radio_access;

esp_err_t spi_shared_bus_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const gpio_num_t chip_selects[] = {
        BOARD_SD_CS_GPIO,     /* SD */
        BOARD_NRF24_CSN_GPIO, /* nRF24 */
        BOARD_CC1101_CS_GPIO, /* CC1101 */
    };
    uint64_t cs_mask = 0;
    for (size_t i = 0; i < sizeof(chip_selects) / sizeof(chip_selects[0]); i++) {
        cs_mask |= 1ULL << chip_selects[i];
        gpio_set_level(chip_selects[i], 1);
    }

    gpio_config_t cs_config = {
        .pin_bit_mask = cs_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cs_config);
    if (err != ESP_OK) {
        return err;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = SHARED_SPI_MOSI,
        .miso_io_num = SHARED_SPI_MISO,
        .sclk_io_num = SHARED_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    err = spi_bus_initialize(SHARED_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        s_ready = true;
    }
    return err;
}

bool spi_shared_bus_is_ready(void)
{
    return s_ready;
}

void spi_shared_bus_set_radio_access(bool enabled)
{
    s_radio_access = enabled;
}

bool spi_shared_bus_radio_access_allowed(void)
{
    return !s_ready || s_radio_access;
}

spi_host_device_t spi_shared_bus_host(void)
{
    return SHARED_SPI_HOST;
}
