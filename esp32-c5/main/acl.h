/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#define ACL_MAX 32

typedef enum {
    ACL_KIND_AUTO  = 0,  /* unspecified — treat MAC as either */
    ACL_KIND_STA   = 1,
    ACL_KIND_BSSID = 2,
} acl_kind_t;

esp_err_t acl_init(void);

/* Whitelist: MACs that must NEVER be deauth'd. */
int  acl_wl_add(const uint8_t *mac, acl_kind_t kind);    /* 0 ok, -1 full, -2 dup, -3 persistence failure */
int  acl_wl_remove(const uint8_t *mac);                  /* 0 ok, -1 not found, -2 persistence failure */
int  acl_wl_clear(void);                                 /* 0 ok, -1 persistence failure */
bool acl_wl_contains(const uint8_t *mac);
void acl_wl_list(void);

/* Blacklist: MACs that should ALWAYS be deauth'd when seen. */
int  acl_bl_add(const uint8_t *mac, acl_kind_t kind);
int  acl_bl_remove(const uint8_t *mac);
int  acl_bl_clear(void);
bool acl_bl_contains(const uint8_t *mac);
void acl_bl_list(void);

/* Iteration of blacklist (for the parallel attack loop). Returns -1 when done. */
int  acl_bl_get(int idx, uint8_t *mac_out, acl_kind_t *kind_out);
int  acl_bl_count(void);

/* Helpers. */
int  acl_parse_mac(const char *s, uint8_t *out);  /* "aa:bb:cc:dd:ee:ff", returns 0 ok */
