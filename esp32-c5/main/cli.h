/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

void cli_init(void);

/* Bytes from any transport — assembles lines and queues complete commands. */
void cli_feed(const char *bytes, size_t len);
