/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "io.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SINKS 4

typedef struct {
    io_sink_fn fn;
    void      *ctx;
} sink_t;

static sink_t s_sinks[MAX_SINKS];
static int    s_sink_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_sink_count; i++) {
        s_sinks[i].fn(buf, (size_t)n, s_sinks[i].ctx);
    }
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
    return n;
}

esp_err_t io_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    s_sink_count = 0;
    esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

int io_register_sink(io_sink_fn fn, void *ctx)
{
    if (!fn || !s_mutex) return -1;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    int rc = -1;
    if (s_sink_count < MAX_SINKS) {
        s_sinks[s_sink_count].fn = fn;
        s_sinks[s_sink_count].ctx = ctx;
        s_sink_count++;
        rc = 0;
    }
    xSemaphoreGiveRecursive(s_mutex);
    return rc;
}

void io_vlog(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_sink_count; i++) {
        s_sinks[i].fn(buf, (size_t)n, s_sinks[i].ctx);
    }
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

void io_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    io_vlog(fmt, ap);
    va_end(ap);
}
