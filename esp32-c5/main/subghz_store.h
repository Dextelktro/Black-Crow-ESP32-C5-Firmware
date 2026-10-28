/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 * Sub-GHz ".sub" file store (Flipper-compatible RAW format).
 *
 * The Flipper keeps .sub files on its SD card; this hardware may or may not
 * have an SD card fitted, so the destination is USER-SELECTED and there is no
 * silent default: a save/list is refused until the user picks LittleFS or SD.
 * SD selected but absent is an explicit error, never a quiet fallback.
 *
 * Files live under "<target>/subghz/<name>.sub":
 *   LittleFS -> /fs/subghz/<name>.sub   (internal flash)
 *   SD       -> /sdcard/subghz/<name>.sub
 *
 * File content (RAW), matching Flipper:
 *   Filetype: Flipper SubGhz RAW File
 *   Version: 1
 *   Frequency: <hz>
 *   Preset: <FuriHalSubGhzPreset...>
 *   Protocol: RAW
 *   RAW_Data: <signed-us> <signed-us> ...   (one or more lines)
 * ============================================================ */

typedef enum {
    SUBGHZ_STORE_UNSET = 0,   /* no destination chosen yet */
    SUBGHZ_STORE_LITTLEFS,
    SUBGHZ_STORE_SD,
} subghz_store_target_t;

#define SUBGHZ_STORE_NAME_MAX 48
#define SUBGHZ_STORE_MAX_TIMINGS 1024   /* explicit cap (matches capture) */

esp_err_t subghz_store_init(void);   /* load persisted target from NVS */

subghz_store_target_t subghz_store_get_target(void);
const char *subghz_store_target_name(subghz_store_target_t target);

/* Whether the CURRENT target is usable right now (LittleFS mountable, or SD
 * present+mounted). UNSET returns false. Reported separately from the target so
 * `storage show` can say e.g. target=sd, available=false. */
bool subghz_store_target_available(void);

/* Persist the destination. SUBGHZ_STORE_SD is accepted even if no card is
 * present right now (it is validated at save time). */
esp_err_t subghz_store_set_target(subghz_store_target_t target);

/* Save a RAW capture. Returns:
 *   ESP_ERR_INVALID_STATE  target is UNSET (user must choose first)
 *   ESP_ERR_NOT_FOUND      target is SD but no card is mounted
 *   ESP_ERR_INVALID_ARG    bad name / empty capture
 * `preset_flipper_name` is the ".sub" Preset string (subghz_preset_t.flipper_name). */
esp_err_t subghz_store_save_raw(const char *name, const int32_t *durations,
                                size_t count, uint32_t freq_hz,
                                const char *preset_flipper_name);

typedef void (*subghz_store_list_cb)(const char *name, size_t size, void *ctx);

/* List .sub files in the active target's subghz directory. */
esp_err_t subghz_store_list(subghz_store_list_cb cb, void *ctx);
esp_err_t subghz_store_delete(const char *name);

/* Load a RAW .sub for emulation. Parses Frequency, Preset and all RAW_Data
 * lines into `out` (up to `cap` timings). Fills *out_count, *out_freq, and the
 * preset name (into preset_out). Same target/name rules and errors as save.
 * Returns ESP_ERR_INVALID_RESPONSE if the file is not a usable RAW capture. */
esp_err_t subghz_store_load_raw(const char *name, int32_t *out, size_t cap,
                                size_t *out_count, uint32_t *out_freq,
                                char *preset_out, size_t preset_cap);

/* ---- Protocol "Key File" (.sub with Protocol/Bit/Key/TE) ---- */

typedef enum {
    SUBGHZ_FILE_NONE = 0,   /* not found / unreadable / no Protocol line */
    SUBGHZ_FILE_RAW,        /* Protocol: RAW   (RAW_Data timings)        */
    SUBGHZ_FILE_KEY,        /* Protocol: <name> + Bit/Key (decoded)      */
} subghz_file_kind_t;

/* Classify a stored .sub as RAW or KEY (reads the Protocol: line). */
subghz_file_kind_t subghz_store_classify(const char *name);

/* Save a decoded protocol as a Flipper SubGhz Key File. */
esp_err_t subghz_store_save_key(const char *name, const char *proto_name,
                                uint16_t bits, uint64_t key, uint32_t te,
                                uint32_t freq_hz, const char *preset_flipper_name);

/* Load a Key File's fields. Any out-pointer may be NULL. */
esp_err_t subghz_store_load_key(const char *name,
                                char *proto_out, size_t proto_cap,
                                uint16_t *bits, uint64_t *key, uint32_t *te,
                                uint32_t *freq, char *preset_out, size_t preset_cap);