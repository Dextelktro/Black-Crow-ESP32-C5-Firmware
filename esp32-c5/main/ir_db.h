#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "ir_proto.h"

/* ============================================================
 * Built-in universal remote libraries, converted from Flipper
 * .ir assets. Entries are keyed by category and button name.
 *
 * Entries are looked up by (category, button-name). The
 * "universal" action sweeps every entry for that pair, exactly
 * like a Flipper universal remote brute-forces brands.
 * ============================================================ */

typedef enum {
    IR_CAT_TV = 0,
    IR_CAT_AC,
    IR_CAT_PROJECTOR,
    IR_CAT_LED,
    IR_CAT_FAN,
    IR_CAT_AUDIO,
    IR_CAT_COUNT,
} ir_cat_t;

typedef struct {
    ir_cat_t    cat;
    const char *button;   /* canonical: power/vol_up/vol_dn/mute/ch_up/ch_dn/... */
    const char *brand;
    ir_proto_t  proto;    /* IR_PROTO_RAW => raw timing signal (see raw/raw_len) */
    uint32_t    address;
    uint32_t    command;
    uint8_t     bits;     /* Sony: 12/15/20, else 0 */
    const uint32_t *raw;  /* raw signals only (NULL for parsed) */
    uint16_t    raw_len;  /* raw timing count */
    uint16_t    carrier;  /* raw carrier Hz */
    float       duty;     /* raw duty cycle; 0 selects the 0.33 default */
} ir_db_entry_t;

const char *ir_cat_name(ir_cat_t c);
ir_cat_t    ir_cat_from_name(const char *s);

/* Number of codes stored for a category+button. */
int  ir_db_count(ir_cat_t cat, const char *button);

/* Fetch the idx-th code for a category+button, or NULL if out of range. */
const ir_db_entry_t *ir_db_get(ir_cat_t cat, const char *button, int idx);
