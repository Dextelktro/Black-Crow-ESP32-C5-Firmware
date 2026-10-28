/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>

#include "driver/spi_master.h"
#include "esp_err.h"

esp_err_t spi_shared_bus_init(void);
bool spi_shared_bus_is_ready(void);
spi_host_device_t spi_shared_bus_host(void);
void spi_shared_bus_set_radio_access(bool enabled);
bool spi_shared_bus_radio_access_allowed(void);
