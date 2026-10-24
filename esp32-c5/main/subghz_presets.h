#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 * CC1101 modulation presets, ported byte-for-byte from the Flipper Zero
 * firmware (lib/subghz/devices/cc1101_configs.c). Each preset is a list of
 * register/value writes followed by an 8-byte PA table. Stored in flash
 * (const) — zero internal RAM cost.
 * ============================================================ */

typedef struct {
    uint8_t reg;
    uint8_t val;
} cc1101_reg_pair_t;

typedef enum {
    SUBGHZ_PRESET_OOK_270 = 0,  /* OOK, 270 kHz RX bandwidth, async */
    SUBGHZ_PRESET_OOK_650,      /* OOK, 650 kHz RX bandwidth, async */
    SUBGHZ_PRESET_2FSK_238,     /* 2-FSK, 2.38 kHz deviation, async */
    SUBGHZ_PRESET_2FSK_476,     /* 2-FSK, 47.6 kHz deviation, async */
    SUBGHZ_PRESET_COUNT,
} subghz_preset_id_t;

typedef struct {
    const char *name;                 /* short CLI name, e.g. "ook650" */
    const char *flipper_name;         /* .sub "Preset:" value */
    const cc1101_reg_pair_t *regs;    /* terminated by {0, 0} */
    uint8_t patable[8];
    bool fm;                          /* modulation: false = AM/OOK, true = 2FSK.
                                       * Explicit (not inferred from enum order)
                                       * so decode/TX stay correct if the table
                                       * is ever reordered or extended. */
} subghz_preset_t;

/* Returns NULL for an out-of-range id. */
const subghz_preset_t *subghz_preset_get(subghz_preset_id_t id);

/* Match a short CLI name ("ook270"/"ook650"/"2fsk238"/"2fsk476") OR a Flipper
 * ".sub" preset name. Returns NULL if unknown. */
const subghz_preset_t *subghz_preset_from_name(const char *name);

/* Write every register pair then the PA table to the CC1101. The chip must be
 * awake and in IDLE. Returns the first error encountered. */
esp_err_t subghz_preset_apply(const subghz_preset_t *preset);
