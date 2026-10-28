/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "targets.h"

/* Mode untuk deauth-all */
typedef enum {
    DAUTH_ALL_MODE_SKIP = 0,     /* Skip AP jika ada WL STA di sana (default) */
    DAUTH_ALL_MODE_UNICAST,      /* Downgrade ke unicast per STA non-WL */
} dauth_all_mode_t;

void attack_init(void);
bool attack_start(uint32_t duration_seconds);
bool attack_nuke_start(uint32_t duration_seconds);

/* Deauth-all dengan mode default (SKIP) */
bool attack_deauth_all_start(uint32_t duration_seconds);

/* Deauth-all dengan mode eksplisit */
bool attack_deauth_all_start_mode(uint32_t duration_seconds,
                                  dauth_all_mode_t mode);

void attack_deauth_all_rescan(void);
void attack_stop(void);
bool attack_is_running(void);
void attack_print_status(void);
