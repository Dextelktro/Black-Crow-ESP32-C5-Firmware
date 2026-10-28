/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "esp_err.h"

/* A transport's output sink. Called from any task; must be reentrant-safe
 * (use a ringbuffer or stream buffer internally). */
typedef void (*io_sink_fn)(const char *buf, size_t len, void *ctx);

/* Set up the broadcast bus. Call once at boot before transports register. */
esp_err_t io_init(void);

/* Register a transport's output sink. Up to 4 sinks supported. */
int io_register_sink(io_sink_fn fn, void *ctx);

/* Broadcast a printf-style line to every registered sink. */
void io_log(const char *fmt, ...);
void io_vlog(const char *fmt, va_list ap);

/* Push raw bytes received from any transport into the CLI line buffer.
 * Defined here for symmetry, implemented in cli.c. */
void cli_feed(const char *bytes, size_t len);
