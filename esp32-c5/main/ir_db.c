#include "ir_db.h"

#include <string.h>
#include <strings.h>

static const char *const k_cat_names[IR_CAT_COUNT] = {
    [IR_CAT_TV]        = "tv",
    [IR_CAT_AC]        = "ac",
    [IR_CAT_PROJECTOR] = "projector",
    [IR_CAT_LED]       = "led",
    [IR_CAT_FAN]       = "fan",
    [IR_CAT_AUDIO]     = "audio",
};

const char *ir_cat_name(ir_cat_t c)
{
    if (c < 0 || c >= IR_CAT_COUNT) return "?";
    return k_cat_names[c];
}

ir_cat_t ir_cat_from_name(const char *s)
{
    if (!s) return IR_CAT_COUNT;
    for (int i = 0; i < IR_CAT_COUNT; i++) {
        if (strcasecmp(s, k_cat_names[i]) == 0) return (ir_cat_t)i;
    }
    return IR_CAT_COUNT;
}

/* Raw TV timing arrays (file scope) referenced by the raw entries below. */
#include "ir_db_tv_raw.inc"
#include "ir_db_ac_raw.inc"
#include "ir_db_projector_raw.inc"
#include "ir_db_led_raw.inc"
#include "ir_db_fan_raw.inc"
#include "ir_db_audio_raw.inc"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const ir_db_entry_t k_db[] = {
    /* ---------------- TV ----------------
     * 765 codes generated from the Flipper universal tv.ir library
     * (450 parsed incl. Kaseikyo + 315 raw). See ir_db_tv.inc. */
#include "ir_db_tv.inc"

    /* ---------------- AC / PROJECTOR / LED / FAN / AUDIO --------------- */
#include "ir_db_ac.inc"
#include "ir_db_projector.inc"
#include "ir_db_led.inc"
#include "ir_db_fan.inc"
#include "ir_db_audio.inc"
};
#pragma GCC diagnostic pop

#define K_DB_N (sizeof(k_db) / sizeof(k_db[0]))

int ir_db_count(ir_cat_t cat, const char *button)
{
    if (!button) return 0;
    int n = 0;
    for (size_t i = 0; i < K_DB_N; i++) {
        if (k_db[i].cat == cat && strcasecmp(k_db[i].button, button) == 0) n++;
    }
    return n;
}

const ir_db_entry_t *ir_db_get(ir_cat_t cat, const char *button, int idx)
{
    if (!button || idx < 0) return NULL;
    int n = 0;
    for (size_t i = 0; i < K_DB_N; i++) {
        if (k_db[i].cat != cat || strcasecmp(k_db[i].button, button) != 0) {
            continue;
        }
        if (n == idx) return &k_db[i];
        n++;
    }
    return NULL;
}
