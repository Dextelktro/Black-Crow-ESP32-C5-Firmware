#include "subghz.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_config.h"
#include "cc1101.h"
#include "cc1101_regs.h"
#include "io.h"
#include "subghz_capture.h"
#include "subghz_presets.h"
#include "subghz_protocol.h"
#include "subghz_store.h"
#include "subghz_tx.h"

#define SUBGHZ_DEFAULT_FREQ 433920000UL
#define SUBGHZ_DEFAULT_PRESET SUBGHZ_PRESET_OOK_650

/* Spectrum sweep */
#define SUBGHZ_SPEC_MAX_BINS   96
#define SUBGHZ_SPEC_CHUNK      24    /* bins per SPECBIN line (fits io_log 256B) */
#define SUBGHZ_SPEC_DEF_SPAN   2000000UL
#define SUBGHZ_SPEC_DEF_BINS   64
#define SUBGHZ_SPEC_MIN_SPAN   100000UL
#define SUBGHZ_SPEC_MAX_SPAN   20000000UL
#define SUBGHZ_RSSI_FLOOR      (-128)
/* Frequency analyzer, ported faithfully from Flipper's
 * subghz_frequency_analyzer_worker.c: a -93 dBm trigger, a wide coarse hop then
 * a narrow ±300 kHz / 20 kHz fine scan. The narrow fine bandwidth is what both
 * resolves the carrier and rejects noise, so no extra sideband-margin test is
 * needed (that only added latency and could drop weak real signals). */
#define SUBGHZ_ANALYZER_MIN_RSSI (-93)   /* SUBGHZ_FREQUENCY_ANALYZER_THRESHOLD */
#define SUBGHZ_ANALYZER_REFINE_RADIUS_HZ 300000
#define SUBGHZ_ANALYZER_REFINE_STEP_HZ   20000
/* RX bandwidth (MDMCFG4) for the two stages, verbatim from Flipper: wide for the
 * coarse hop, narrow for the fine scan. */
#define SUBGHZ_ANALYZER_BW_COARSE 0x17   /* 650 kHz (0b00010111) */
#define SUBGHZ_ANALYZER_BW_FINE   0xF7   /* 58 kHz  (0b11110111) */

/* Read RAW */
#define SUBGHZ_RAW_CHUNK        16     /* durations per RAWBIN line (fits 256B) */
#define SUBGHZ_DEFAULT_RSSI_THR (-90)  /* matches Flipper Unleashed RAW default */
#define SUBGHZ_RSSI_SQUELCH_OFF (-127) /* threshold <= this disables the squelch */
#define SUBGHZ_RSSI_POLL_MS     2      /* RSSI sampling period */
#define SUBGHZ_RSSI_CAPTURE_HOLD_MS 400 /* RMT idle-stop + callback latency */
#define SUBGHZ_RSSI_SAMPLE_COUNT 3     /* median RSSI samples for sweep measurements */
#define SUBGHZ_READ_DEDUPE_MS 1500        /* suppress identical repeats while a
                                            * remote key is held down */
#define SUBGHZ_RAW_STRICT_DEDUPE_MS 1500
#define SUBGHZ_RAW_DEDUPE_TIMING_TOLERANCE_PCT 30
#define SUBGHZ_RAW_DEDUPE_MIN_TOLERANCE_US 50
/* Synthesized silence inserted between captured frames in the continuous Read
 * RAW recording. The frame splitter trims the real inter-frame gap, so one is
 * re-inserted on replay — long enough that a receiver sees distinct signals,
 * short enough to keep the recording compact. (Flipper keeps the real gap; we
 * approximate it since our pipeline segments frames for the decoder.) */
#define SUBGHZ_RAWREC_GAP_US 15000

_Static_assert(SUBGHZ_RSSI_SAMPLE_COUNT == 3,
               "median_of_three requires exactly three RSSI samples");

/* TX / emulate */
#define SUBGHZ_GDO0_GPIO          BOARD_CC1101_GDO0_GPIO  /* CC1101 GDO0 (async serial data line) */
#define SUBGHZ_TX_DEFAULT_REPEATS 3
#define SUBGHZ_TX_MAX_REPEATS     100    /* bound total TX time */
#define SUBGHZ_TX_GAP_MS          20     /* silence between repeated frames */

/* Brute force */
#define SUBGHZ_BRUTE_MAX_BITS     20     /* 2^20 codes cap (keeps run bounded) */
#define SUBGHZ_BRUTE_DEF_REPEATS  2
#define SUBGHZ_BRUTE_PROGRESS_MASK 0x3FU /* log progress every 64 codes */
#define SUBGHZ_BRUTE_FRAME_MAX    128

typedef enum {
    SUBGHZ_MODE_IDLE = 0,
    SUBGHZ_MODE_ANALYZER,
    SUBGHZ_MODE_SPECTRUM,
    SUBGHZ_MODE_READ_RAW,
    SUBGHZ_MODE_READ,       /* capture + decode known protocols */
    SUBGHZ_MODE_TX,
    SUBGHZ_MODE_BRUTE,      /* iterate a protocol code space, TX each */
} subghz_mode_t;

static bool s_ready;
static volatile bool s_radio_fault;
static uint32_t s_freq = SUBGHZ_DEFAULT_FREQ;
static subghz_preset_id_t s_preset = SUBGHZ_DEFAULT_PRESET;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_mode_lock;

/* Sweep worker state */
static volatile subghz_mode_t s_mode = SUBGHZ_MODE_IDLE;
static volatile bool s_run;
static volatile bool s_worker_alive;   /* authoritative "worker task exists" */
static uint32_t s_spec_center;
static uint32_t s_spec_span;
static int s_spec_bins;

/* Read RAW state */
static volatile int s_rssi_threshold = SUBGHZ_DEFAULT_RSSI_THR;
static volatile int s_last_rssi = SUBGHZ_RSSI_FLOOR;
static portMUX_TYPE s_rssi_mux = portMUX_INITIALIZER_UNLOCKED;
static TickType_t s_rssi_last_above_tick;
static bool s_rssi_seen_above;
static int32_t *s_raw_last;              /* PSRAM, kept for save (Stage 4)   */
static size_t s_raw_last_count;
static uint32_t s_raw_last_freq;
static subghz_preset_id_t s_raw_last_preset;
static bool s_raw_last_truncated;        /* last capture may have lost edges  */
static uint32_t s_raw_seq;
static subghz_decoded_t s_last_decoded;  /* last READ-mode decode (if any)    */
static bool s_have_decoded;              /* s_last_decoded matches s_raw_last  */
static subghz_decoded_t s_read_last_reported;
static TickType_t s_read_last_reported_tick;
static bool s_read_have_reported;
static bool s_readraw_strict;
static bool s_raw_have_reported;
static TickType_t s_raw_last_reported_tick;
static uint32_t s_rssi_read_errors;      /* diagnostic: failed RSSI reads    */
static uint32_t s_squelch_opens;         /* diagnostic: bursts past squelch  */

/* TX request (set before start_mode(TX)) */
static const int32_t *s_tx_src;          /* PSRAM: s_raw_last or s_emu_buf   */
static size_t s_tx_count;
static uint32_t s_tx_freq;
static subghz_preset_id_t s_tx_preset;
static int s_tx_repeats;
static uint32_t s_tx_gap_ms;
static int32_t *s_emu_buf;               /* PSRAM: .sub load buffer for emulate */

/* Brute-force request/state (bits <= 20 so a 32-bit index/code is enough). */
static subghz_proto_id_t s_brute_proto;
static uint16_t s_brute_bits;
static uint32_t s_brute_idx;             /* next code to send (resume point) */
static uint32_t s_brute_end;             /* inclusive last code */
static int s_brute_repeats;
static uint32_t s_brute_freq;            /* freq the run started on (for resume) */

/* Frequency Analyzer hop list — representative frequencies across the three
 * allowed bands (all pass subghz_frequency_valid). */
static const uint32_t s_hoplist[] = {
    300000000UL, 303875000UL, 304250000UL, 310000000UL, 315000000UL,
    318000000UL, 330000000UL, 345000000UL,
    390000000UL, 418000000UL, 433075000UL, 433420000UL, 433920000UL,
    434420000UL, 434775000UL, 438900000UL, 464000000UL,
    868350000UL, 868950000UL, 915000000UL, 925000000UL,
};
#define SUBGHZ_HOP_N (sizeof(s_hoplist) / sizeof(s_hoplist[0]))

/* Focused spectrum bands offered by the UI (center + span, in Hz). All lie
 * inside subghz_frequency_valid() ranges. Order matches SUBGHZ_SPEC_BAND_COUNT
 * and the app's picker (option 1..6). */
typedef struct {
    const char *label;
    uint32_t center;
    uint32_t span;
} subghz_spec_band_t;

static const subghz_spec_band_t s_spec_band_tbl[SUBGHZ_SPEC_BAND_COUNT] = {
    {"315", 315000000UL, 6000000UL},  /* 312 - 318   MHz */
    {"390", 390000000UL, 6000000UL},  /* 387 - 393   MHz */
    {"415", 415000000UL, 6000000UL},  /* 412 - 418   MHz */
    {"433", 433450000UL, 6900000UL},  /* 430 - 436.9 MHz */
    {"868", 868000000UL, 6000000UL},  /* 865 - 871   MHz */
    {"915", 915000000UL, 6000000UL},  /* 912 - 918   MHz */
};

/* Flipper Sub-GHz allowed frequency ranges, in Hz (furi_hal_subghz). */
static const struct {
    uint32_t lo;
    uint32_t hi;
} s_bands[] = {
    {299999755UL, 348000335UL},
    {386999938UL, 464000000UL},
    {778999847UL, 928000000UL},
};

static int median_of_three(int a, int b, int c)
{
    if (a > b) { int tmp = a; a = b; b = tmp; }
    if (b > c) { int tmp = b; b = c; c = tmp; }
    if (a > b) { int tmp = a; a = b; b = tmp; }
    return b;
}

static void update_rssi_squelch(int rssi)
{
    int threshold = s_rssi_threshold;
    TickType_t now = xTaskGetTickCount();
    portENTER_CRITICAL(&s_rssi_mux);
    if (threshold <= SUBGHZ_RSSI_SQUELCH_OFF || rssi >= threshold) {
        s_rssi_seen_above = true;
        s_rssi_last_above_tick = now;
    }
    portEXIT_CRITICAL(&s_rssi_mux);
}

static bool rssi_squelch_is_open(void)
{
    if (s_rssi_threshold <= SUBGHZ_RSSI_SQUELCH_OFF) return true;
    TickType_t now = xTaskGetTickCount();
    bool recently_above;
    portENTER_CRITICAL(&s_rssi_mux);
    TickType_t age = now - s_rssi_last_above_tick;
    TickType_t hold = pdMS_TO_TICKS(SUBGHZ_RSSI_CAPTURE_HOLD_MS);
    if (hold == 0) hold = 1;
    recently_above = s_rssi_seen_above && age <= hold;
    portEXIT_CRITICAL(&s_rssi_mux);
    return recently_above;
}

bool subghz_frequency_valid(uint32_t hz)
{
    for (size_t i = 0; i < sizeof(s_bands) / sizeof(s_bands[0]); i++) {
        if (hz >= s_bands[i].lo && hz <= s_bands[i].hi) return true;
    }
    return false;
}

esp_err_t subghz_init(void)
{
    if (s_ready) return ESP_OK;
    if (!cc1101_is_present()) {
        io_log("subghz: CC1101 not present\r\n");
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_mode_lock) {
        s_mode_lock = xSemaphoreCreateMutex();
        if (!s_mode_lock) return ESP_ERR_NO_MEM;
    }
    s_freq = SUBGHZ_DEFAULT_FREQ;
    s_preset = SUBGHZ_DEFAULT_PRESET;
    s_radio_fault = false;
    subghz_store_init();   /* load persisted storage target (NVS) */
    s_ready = true;
    return ESP_OK;
}

bool subghz_module_ready(void)
{
    return s_ready;
}

uint32_t subghz_get_frequency(void)
{
    return s_freq;
}

subghz_preset_id_t subghz_get_preset(void)
{
    return s_preset;
}

esp_err_t subghz_set_frequency(uint32_t hz)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    /* Refuse mid-session: a running sweep/capture already tuned the chip, and
     * changing s_freq now would mislabel captured bursts with a frequency the
     * signal was never received on. Stop first, then retune. */
    if (subghz_is_busy()) {
        io_log("subghz: ERR busy (stop before changing frequency)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!subghz_frequency_valid(hz)) {
        io_log("subghz: ERR freq-out-of-band %lu\r\n", (unsigned long)hz);
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_freq = hz;
    xSemaphoreGive(s_lock);
    io_log("subghz: FREQ %lu\r\n", (unsigned long)hz);
    return ESP_OK;
}

/* A preset returned by subghz_preset_from_name() is an element of the
 * contiguous preset table, so its id is the pointer offset from element 0. */
static subghz_preset_id_t preset_id_of(const subghz_preset_t *p)
{
    return (subghz_preset_id_t)(p - subghz_preset_get(0));
}

esp_err_t subghz_set_preset(const char *name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (subghz_is_busy()) {
        io_log("subghz: ERR busy (stop before changing preset)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    const subghz_preset_t *preset = subghz_preset_from_name(name);
    if (!preset) {
        io_log("subghz: ERR unknown-preset %s\r\n", name ? name : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    subghz_preset_id_t id = preset_id_of(preset);
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_preset = id;
    xSemaphoreGive(s_lock);
    io_log("subghz: PRESET %s\r\n", preset->name);
    return ESP_OK;
}

/* Wake + reset + apply a preset and frequency, leaving the chip in IDLE.
 * Caller holds s_lock. */
static esp_err_t configure_radio(subghz_preset_id_t preset_id, uint32_t freq)
{
    esp_err_t err = cc1101_wake();
    if (err != ESP_OK) return err;
    err = cc1101_reset();
    if (err != ESP_OK) return err;
    const subghz_preset_t *preset = subghz_preset_get(preset_id);
    if (!preset) return ESP_ERR_INVALID_STATE;
    err = subghz_preset_apply(preset);
    if (err != ESP_OK) return err;
    return cc1101_set_frequency(freq);
}

static esp_err_t wait_for_radio_state(uint8_t expected, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        uint8_t state = 0xFF;
        esp_err_t err = cc1101_read_marcstate(&state);
        if (err == ESP_OK && state == expected) return ESP_OK;
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

/* Configure with the current (read/analyzer/spectrum) preset + frequency. */
static esp_err_t session_configure(void)
{
    return configure_radio(s_preset, s_freq);
}

/* Return the chip to IDLE then SPWD, retrying a few times: at stop the
 * shared-bus radio gate may be briefly held (e.g. by an SD transaction),
 * which would fail the sleep and leave the CC1101 powered in RX. */
static esp_err_t subghz_power_down(void)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        err = cc1101_idle();
        if (err == ESP_OK) err = cc1101_sleep();
        if (err == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    err = cc1101_emergency_shutdown();
    if (err == ESP_OK) {
        io_log("subghz: WARN emergency radio shutdown used\r\n");
        return ESP_OK;
    }
    s_radio_fault = true;
    io_log("subghz: ERR power-down incomplete (%s)\r\n", esp_err_to_name(err));
    return err;
}

static void subghz_tx_cleanup(void)
{
    bool radio_idle = subghz_power_down() == ESP_OK;
    subghz_tx_end(radio_idle);
    if (!radio_idle) {
        io_log("subghz: CRITICAL GDO0 held low; radio state unconfirmed\r\n");
    }
}

static esp_err_t subghz_capture_stop_and_wait(void)
{
    esp_err_t err = subghz_capture_stop();
    if (err == ESP_ERR_TIMEOUT) {
        io_log("subghz: WARN waiting for RX task teardown\r\n");
    }
    while (err == ESP_ERR_TIMEOUT) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = subghz_capture_stop();
    }
    return err;
}

/* Retune to hz, enter RX, wait for RX + RSSI to settle, and return RSSI in
 * dBm. Returns SUBGHZ_RSSI_FLOOR on any SPI/tune error. The settle uses
 * vTaskDelay (not a busy wait) so a long sweep yields the CPU every bin and
 * cannot starve the idle task / trip the task watchdog. Runs only inside the
 * sweep worker (serialized with other radio users by s_mode != IDLE). */
static int measure_rssi_at(uint32_t hz)
{
    if (cc1101_set_frequency(hz) != ESP_OK) return SUBGHZ_RSSI_FLOOR;
    if (cc1101_strobe(CC1101_STROBE_SRX, NULL) != ESP_OK) return SUBGHZ_RSSI_FLOOR;
    /* Confirm RX before sampling; if it never settles, report the floor rather
     * than a stale/calibrating sample for this bin/hop. */
    bool in_rx = false;
    for (int i = 0; i < 4; i++) {
        uint8_t st = 0;
        if (cc1101_read_marcstate(&st) == ESP_OK && st == CC1101_MARC_STATE_RX) {
            in_rx = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!in_rx) return SUBGHZ_RSSI_FLOOR;

    TickType_t sample_delay = pdMS_TO_TICKS(SUBGHZ_RSSI_POLL_MS);
    if (sample_delay == 0) sample_delay = 1;
    vTaskDelay(sample_delay);

    int samples[SUBGHZ_RSSI_SAMPLE_COUNT];
    for (size_t i = 0; i < SUBGHZ_RSSI_SAMPLE_COUNT; i++) {
        uint8_t raw = 0;
        if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
            return SUBGHZ_RSSI_FLOOR;
        }
        samples[i] = cc1101_rssi_raw_to_dbm(raw);
        if (i + 1 < SUBGHZ_RSSI_SAMPLE_COUNT) vTaskDelay(sample_delay);
    }
    return median_of_three(samples[0], samples[1], samples[2]);
}

static void spectrum_sweep(uint32_t seq)
{
    uint32_t center = s_spec_center;
    uint32_t span = s_spec_span;
    int n = s_spec_bins;
    if (n < 1 || n > SUBGHZ_SPEC_MAX_BINS) n = SUBGHZ_SPEC_DEF_BINS;
    uint32_t step = span / (uint32_t)n;
    if (step == 0) step = 1;
    uint32_t start = center - span / 2;

    int16_t vals[SUBGHZ_SPEC_MAX_BINS];
    int peak = SUBGHZ_RSSI_FLOOR;
    uint32_t peakfq = center;
    for (int i = 0; i < n && s_run; i++) {
        uint32_t f = start + (uint32_t)i * step + step / 2;
        int r = subghz_frequency_valid(f) ? measure_rssi_at(f) : SUBGHZ_RSSI_FLOOR;
        vals[i] = (int16_t)r;
        if (r > peak) { peak = r; peakfq = f; }
    }
    if (!s_run) return;

    io_log("subghz: SPEC seq=%lu center=%lu span=%lu step=%lu n=%d peak=%d "
           "peakfq=%lu\r\n",
           (unsigned long)seq, (unsigned long)center, (unsigned long)span,
           (unsigned long)step, n, peak, (unsigned long)peakfq);
    for (int off = 0; off < n; off += SUBGHZ_SPEC_CHUNK) {
        char line[160];
        size_t p = 0;
        int end = off + SUBGHZ_SPEC_CHUNK;
        if (end > n) end = n;
        for (int i = off; i < end; i++) {
            int w = snprintf(line + p, sizeof(line) - p,
                             (i == off) ? "%d" : ",%d", vals[i]);
            if (w < 0 || (size_t)w >= sizeof(line) - p) break;
            p += (size_t)w;
        }
        io_log("subghz: SPECBIN seq=%lu off=%d v=%s\r\n",
               (unsigned long)seq, off, line);
    }
}

/* One fast RSSI sample at hz for an analyzer hop: tune, enter RX, confirm RX,
 * settle 2 ms (Flipper's furi_delay_ms(2)), read ONE sample. No 3-sample median
 * — the analyzer takes the peak across dozens of steps, so per-step smoothing is
 * unnecessary and only adds latency (the user's "delay" complaint). Returns dBm
 * or SUBGHZ_RSSI_FLOOR on error / no settle. */
static int measure_rssi_fast(uint32_t hz)
{
    if (cc1101_set_frequency(hz) != ESP_OK) return SUBGHZ_RSSI_FLOOR;
    if (cc1101_strobe(CC1101_STROBE_SRX, NULL) != ESP_OK) return SUBGHZ_RSSI_FLOOR;
    bool in_rx = false;
    for (int i = 0; i < 4; i++) {
        uint8_t st = 0;
        if (cc1101_read_marcstate(&st) == ESP_OK && st == CC1101_MARC_STATE_RX) {
            in_rx = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!in_rx) return SUBGHZ_RSSI_FLOOR;
    vTaskDelay(pdMS_TO_TICKS(2));
    uint8_t raw = 0;
    if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
        return SUBGHZ_RSSI_FLOOR;
    }
    return cc1101_rssi_raw_to_dbm(raw);
}

/* Switch the analyzer RX bandwidth (MDMCFG4). The chip must be idle while a
 * demod register is rewritten; the next SRX (in measure_rssi_fast) recalibrates. */
static void analyzer_set_bandwidth(uint8_t mdmcfg4)
{
    (void)cc1101_idle();
    (void)cc1101_write_reg(CC1101_MDMCFG4, mdmcfg4);
}

static void analyzer_scan(void)
{
    /* COARSE: wide 650 kHz, hop the band list, find the strongest frequency. */
    analyzer_set_bandwidth(SUBGHZ_ANALYZER_BW_COARSE);
    int peak = SUBGHZ_RSSI_FLOOR;
    uint32_t peakfq = 0;
    for (size_t i = 0; i < SUBGHZ_HOP_N && s_run; i++) {
        int r = measure_rssi_fast(s_hoplist[i]);
        if (r > peak) { peak = r; peakfq = s_hoplist[i]; }
    }
    if (!s_run) return;
    if (peak < SUBGHZ_ANALYZER_MIN_RSSI || peakfq == 0) {
        io_log("subghz: ANALYZER peakfq=0 rssi=%d\r\n", SUBGHZ_RSSI_FLOOR);
        return;
    }

    /* FINE: narrow 58 kHz, scan ±300 kHz / 20 kHz around the coarse peak. The
     * narrow bandwidth resolves the carrier and rejects wideband noise. */
    analyzer_set_bandwidth(SUBGHZ_ANALYZER_BW_FINE);
    int fine_peak = SUBGHZ_RSSI_FLOOR;
    uint32_t fine_fq = peakfq;
    for (int32_t offset = -SUBGHZ_ANALYZER_REFINE_RADIUS_HZ;
         offset <= SUBGHZ_ANALYZER_REFINE_RADIUS_HZ && s_run;
         offset += SUBGHZ_ANALYZER_REFINE_STEP_HZ) {
        int64_t candidate = (int64_t)peakfq + offset;
        if (candidate < 0 || !subghz_frequency_valid((uint32_t)candidate)) {
            continue;
        }
        int r = measure_rssi_fast((uint32_t)candidate);
        if (r > fine_peak) {
            fine_peak = r;
            fine_fq = (uint32_t)candidate;
        }
    }
    analyzer_set_bandwidth(SUBGHZ_ANALYZER_BW_COARSE);   /* restore for next pass */
    if (!s_run) return;

    /* The narrow fine scan must ALSO clear the threshold; if it does not, the
     * wide coarse hit was noise spread across the band, not a real carrier. */
    if (fine_peak < SUBGHZ_ANALYZER_MIN_RSSI) {
        io_log("subghz: ANALYZER peakfq=0 rssi=%d\r\n", SUBGHZ_RSSI_FLOOR);
        return;
    }
    io_log("subghz: ANALYZER peakfq=%lu rssi=%d\r\n",
           (unsigned long)fine_fq, fine_peak);
}

static bool decoded_frame_matches(const subghz_decoded_t *a,
                                  const subghz_decoded_t *b)
{
    /* TE can vary between captures; identity is protocol, bit count, and key. */
    return a->proto == b->proto && a->bits == b->bits && a->key == b->key;
}

static bool read_should_report(const subghz_decoded_t *decoded)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t dedupe_window = pdMS_TO_TICKS(SUBGHZ_READ_DEDUPE_MS);
    if (s_read_have_reported &&
        decoded_frame_matches(&s_read_last_reported, decoded) &&
        (TickType_t)(now - s_read_last_reported_tick) < dedupe_window) {
        return false;
    }
    s_read_last_reported = *decoded;
    s_read_last_reported_tick = now;
    s_read_have_reported = true;
    return true;
}

static uint64_t raw_duration_magnitude(int32_t duration)
{
    return duration < 0 ? (uint64_t)(-(int64_t)duration) : (uint64_t)duration;
}

static bool raw_timings_match(const int32_t *a, size_t a_count,
                              const int32_t *b, size_t b_count)
{
    if (!a || !b || a_count != b_count) return false;
    for (size_t i = 0; i < a_count; i++) {
        if ((a[i] < 0) != (b[i] < 0)) return false;
        uint64_t a_us = raw_duration_magnitude(a[i]);
        uint64_t b_us = raw_duration_magnitude(b[i]);
        uint64_t diff = a_us > b_us ? a_us - b_us : b_us - a_us;
        uint64_t tolerance =
            (a_us * SUBGHZ_RAW_DEDUPE_TIMING_TOLERANCE_PCT) / 100;
        if (tolerance < SUBGHZ_RAW_DEDUPE_MIN_TOLERANCE_US) {
            tolerance = SUBGHZ_RAW_DEDUPE_MIN_TOLERANCE_US;
        }
        if (diff > tolerance) return false;
    }
    return true;
}

/* Capture callback (runs in the capture rx_task): apply the RSSI squelch,
 * stash the burst as the "last capture" for save/emulate, and stream it out as
 * chunked RAW telemetry. */
static void raw_capture_cb(const int32_t *durations, size_t count,
                           bool truncated, void *ctx)
{
    (void)ctx;

    /* FIX ROOT-CAUSE: capture yang memenuhi seluruh buffer 512-simbol tanpa
     * pernah mencapai idle-gap bersih BUKAN sinyal tunggal nyata — protokol
     * terlebar yang didukung (Phoenix_V2, 52 bit) paling banyak butuh ~108
     * edge, jadi 1024 edge cuma terjadi kalau GDO0 tidak pernah diam karena
     * band memang berisik. Buang tanpa syarat, sebelum squelch/decode apa
     * pun, untuk KEDUA mode. Ini memperbaiki dua gejala sekaligus: noise
     * tampil sebagai "sinyal" di RAW, dan READ tidak pernah decode sama
     * sekali (decode selalu mensyaratkan !truncated — kalau band cukup
     * berisik sampai semua capture truncated, decode tidak pernah dapat
     * kesempatan jalan). */
    if (truncated) {
        return;
    }

    bool read_mode = (s_mode == SUBGHZ_MODE_READ);

    /* Filter noise terakhir yang tersisa untuk RAW: apakah energi RF memang
     * baru saja terlihat (rssi_squelch_is_open(), lihat jendela yang sudah
     * dilebarkan di atas). READ mode tidak perlu ini di jalur DECODED —
     * decode protokol yang berhasil adalah kecocokan presisi puluhan edge
     * dengan timing spesifik, sudah jadi filter noise yang sangat kuat
     * sendirian; menggerbang itu dengan RSSI lagi cuma menambah risiko
     * menolak capture yang justru sudah benar-benar ter-decode, tanpa
     * manfaat keamanan. */
    bool squelch_on = (s_rssi_threshold > SUBGHZ_RSSI_SQUELCH_OFF);
    if (!read_mode && squelch_on && !rssi_squelch_is_open()) {
        return;
    }
    if (squelch_on && !read_mode) s_squelch_opens++;

    bool strict_raw = !read_mode && s_mode == SUBGHZ_MODE_READ_RAW &&
                      s_readraw_strict;

    subghz_decoded_t dec = {0};
    bool decoded = false;
    if (read_mode) {
        const subghz_preset_t *cp = subghz_preset_get(s_preset);
        bool fm = cp && cp->fm;   /* capture modulation (explicit preset flag) */
        decoded = subghz_protocol_decode(durations, count, fm, &dec);
    }
    /* A successful protocol decode is already a precise match of dozens of
     * edges against protocol-specific timing — the strongest possible noise
     * filter — so it is reported on the FIRST decode (no repeat confirmation),
     * matching Flipper's Read, which shows a signal the instant it decodes.
     * READ mode NEVER reports an undecoded "unknown" burst: that path was the
     * main noise source the user saw (any RSSI-qualified burst, with no
     * protocol validation at all, was shown as a signal). Only genuinely
     * decoded remote signals appear now. */
    bool decoded_accepted = decoded;
    bool report_decoded = false;

    /* Three distinct snapshot behaviours share one buffer (s_raw_last):
     *   READ      -> keep the latest complete burst (for save as Key/RAW),
     *   strict RAW -> keep one clean waveform + suppress near-duplicate repeats,
     *   RAW (default) -> CONTINUOUS RECORDER: append every frame (+ a synthesized
     *                    inter-frame gap) into one growing recording, like
     *                    Flipper's Read RAW, so save/transmit cover ALL detected
     *                    signals, not just the last one. */
    bool emit_raw_capture = true;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool preserve_decoded = read_mode && s_have_decoded && !decoded_accepted;
        bool capture_updated = false;
        TickType_t now = xTaskGetTickCount();

        if (read_mode) {
            if (s_raw_last && !preserve_decoded) {
                size_t m = count > SUBGHZ_RAW_MAX_TIMINGS ? SUBGHZ_RAW_MAX_TIMINGS
                                                          : count;
                memcpy(s_raw_last, durations, m * sizeof(int32_t));
                s_raw_last_count = m;
                s_raw_last_freq = s_freq;
                s_raw_last_preset = s_preset;
                s_raw_last_truncated = false;   /* truncated dropped up front */
                capture_updated = true;
            }
            if (decoded_accepted && capture_updated) {
                s_last_decoded = dec;     /* for `save` as a protocol Key File */
                s_have_decoded = true;
                report_decoded = read_should_report(&dec);
            } else if (!preserve_decoded && capture_updated) {
                s_have_decoded = false;
            }
        } else if (strict_raw) {
            TickType_t raw_dedupe_window =
                pdMS_TO_TICKS(SUBGHZ_RAW_STRICT_DEDUPE_MS);
            bool duplicate_raw = s_raw_last && s_raw_have_reported &&
                s_raw_last_count == count && s_raw_last_freq == s_freq &&
                raw_timings_match(s_raw_last, s_raw_last_count, durations, count) &&
                (TickType_t)(now - s_raw_last_reported_tick) < raw_dedupe_window;
            if (duplicate_raw || !s_raw_last) {
                emit_raw_capture = false;
            } else {
                size_t m = count > SUBGHZ_RAW_MAX_TIMINGS ? SUBGHZ_RAW_MAX_TIMINGS
                                                          : count;
                memcpy(s_raw_last, durations, m * sizeof(int32_t));
                s_raw_last_count = m;
                s_raw_last_freq = s_freq;
                s_raw_last_preset = s_preset;
                s_raw_last_truncated = false;
                s_raw_have_reported = true;
                s_raw_last_reported_tick = now;
            }
            s_have_decoded = false;
        } else {   /* recorder */
            if (s_raw_last) {
                size_t pos = s_raw_last_count;
                size_t extra = count + (pos > 0 ? 1u : 0u);   /* +1 for the gap */
                if (pos + extra <= SUBGHZ_RAW_MAX_TIMINGS) {
                    if (pos > 0) {
                        s_raw_last[pos++] = -(int32_t)SUBGHZ_RAWREC_GAP_US;
                    }
                    memcpy(&s_raw_last[pos], durations, count * sizeof(int32_t));
                    s_raw_last_count = pos + count;
                    s_raw_last_freq = s_freq;
                    s_raw_last_preset = s_preset;
                } else {
                    /* Recording buffer full: keep what we have intact and flag
                     * that later signals were dropped (the app warns on save). */
                    s_raw_last_truncated = true;
                }
            }
            s_have_decoded = false;
        }
        xSemaphoreGive(s_lock);
    } else {
        io_log("subghz: ERR capture-lock\r\n");
        if (strict_raw) emit_raw_capture = false;
    }

    if (read_mode) {
        if (report_decoded) {
            io_log("subghz: READ proto=%s bits=%u key=0x%08lX%08lX te=%lu "
                   "fq=%lu rssi=%d\r\n",
                   subghz_protocol_name(dec.proto), dec.bits,
                   (unsigned long)(uint32_t)(dec.key >> 32),
                   (unsigned long)(uint32_t)(dec.key & 0xFFFFFFFFULL),
                   (unsigned long)dec.te, (unsigned long)s_freq, s_last_rssi);
        }
        return;
    }

    if (!emit_raw_capture) return;
    uint32_t seq = ++s_raw_seq;
    const subghz_preset_t *preset = subghz_preset_get(s_preset);
    io_log("subghz: RAW seq=%lu n=%u fq=%lu preset=%s rssi=%d trunc=%d\r\n",
           (unsigned long)seq, (unsigned)count, (unsigned long)s_freq,
           preset ? preset->name : "?", s_last_rssi, truncated ? 1 : 0);
    for (size_t off = 0; off < count; off += SUBGHZ_RAW_CHUNK) {
        char line[200];
        size_t p = 0;
        size_t end = off + SUBGHZ_RAW_CHUNK;
        if (end > count) end = count;
        for (size_t i = off; i < end; i++) {
            int w = snprintf(line + p, sizeof(line) - p,
                             (i == off) ? "%ld" : ",%ld", (long)durations[i]);
            if (w < 0 || (size_t)w >= sizeof(line) - p) break;
            p += (size_t)w;
        }
        io_log("subghz: RAWBIN seq=%lu off=%u v=%s\r\n",
               (unsigned long)seq, (unsigned)off, line);
    }
}

/* Clear worker state and self-delete. */
static void worker_finish(void)
{
    if (xSemaphoreTake(s_mode_lock, portMAX_DELAY) == pdTRUE) {
        s_run = false;
        s_mode = SUBGHZ_MODE_IDLE;
        s_worker_alive = false;
        xSemaphoreGive(s_mode_lock);
    } else {
        io_log("subghz: ERR mode-lock during worker shutdown\r\n");
    }
    vTaskDelete(NULL);
}

static void subghz_worker(void *arg)
{
    (void)arg;
    subghz_mode_t mode = s_mode;

    if (mode == SUBGHZ_MODE_TX) {
        esp_err_t cerr = ESP_ERR_INVALID_STATE;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            cerr = configure_radio(s_tx_preset, s_tx_freq);  /* TX preset+freq */
            xSemaphoreGive(s_lock);
        }
        if (cerr == ESP_OK && !s_run) {
            (void)subghz_power_down();
            worker_finish();
            return;
        }
        if (cerr == ESP_OK) cerr = subghz_tx_begin(SUBGHZ_GDO0_GPIO);
        if (cerr != ESP_OK) {
            io_log("subghz: ERR tx-setup (%s)\r\n", esp_err_to_name(cerr));
            subghz_tx_cleanup();
            worker_finish();
            return;
        }
        if (!s_run) {
            subghz_tx_cleanup();
            worker_finish();
            return;
        }
        esp_err_t stx = cc1101_strobe(CC1101_STROBE_STX, NULL);  /* async TX */
        if (stx == ESP_OK) {
            stx = wait_for_radio_state(CC1101_MARC_STATE_TX, 5);
        }
        if (stx != ESP_OK) {
            /* If the chip never entered TX, driving GDO0 would transmit
             * nothing — don't report a phantom success. */
             io_log("subghz: ERR tx-strobe (%s)\r\n", esp_err_to_name(stx));
             subghz_tx_cleanup();
             worker_finish();
             return;
        }
        if (!s_run) {
             (void)cc1101_idle();
             subghz_tx_cleanup();
            worker_finish();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));              /* let the PA/synth settle */
        if (!s_run) {
            (void)cc1101_idle();
            subghz_tx_cleanup();
            worker_finish();
            return;
        }
        size_t subus = subghz_tx_count_below_min(s_tx_src, s_tx_count);
        if (subus > 0) {
            io_log("subghz: WARN tx-subus n=%u (<5us, clamped; timing not "
                   "guaranteed on this backend)\r\n", (unsigned)subus);
        }
        io_log("subghz: TX START fq=%lu n=%u repeats=%d\r\n",
               (unsigned long)s_tx_freq, (unsigned)s_tx_count, s_tx_repeats);
        int sent = 0;
        for (int r = 0; r < s_tx_repeats && s_run; r++) {
            esp_err_t te = subghz_tx_send(s_tx_src, s_tx_count);
            if (te != ESP_OK) {
                if (s_run) {
                    io_log("subghz: ERR tx-send (%s)\r\n", esp_err_to_name(te));
                }
                break;
            }
            sent++;
            if (r + 1 < s_tx_repeats && s_run) {
                vTaskDelay(pdMS_TO_TICKS(s_tx_gap_ms));
            }
        }
        /* Power down FIRST (retried SIDLE+SPWD) so the chip has left TX and the
         * PA is off BEFORE GDO0 is released to a floating input — otherwise a
         * failed idle could leave the PA following a floating pin (spurious RF).
         * GDO0 is still driven low here, keeping the carrier off meanwhile. */
        subghz_tx_cleanup();
        io_log("subghz: TX DONE sent=%d maxlate_us=%lu\r\n",
               sent, (unsigned long)subghz_tx_max_late_us());
        worker_finish();
        return;
    }

    if (mode == SUBGHZ_MODE_BRUTE) {
        esp_err_t cerr = ESP_ERR_INVALID_STATE;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            cerr = configure_radio(s_tx_preset, s_tx_freq);
            xSemaphoreGive(s_lock);
        }
        if (cerr == ESP_OK && !s_run) {
            (void)subghz_power_down();
            worker_finish();
            return;
        }
        if (cerr == ESP_OK) cerr = subghz_tx_begin(SUBGHZ_GDO0_GPIO);
        if (cerr == ESP_OK && !s_run) {
            (void)subghz_power_down();
            subghz_tx_end(true);
            worker_finish();
            return;
        }
        if (cerr == ESP_OK) cerr = cc1101_strobe(CC1101_STROBE_STX, NULL);
        if (cerr != ESP_OK) {
            io_log("subghz: ERR brute-setup (%s)\r\n", esp_err_to_name(cerr));
            subghz_tx_cleanup();
            worker_finish();
            return;
        }
        if (!s_run) {
            (void)cc1101_idle();
            subghz_tx_cleanup();
            worker_finish();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        if (!s_run) {
            (void)cc1101_idle();
            subghz_tx_cleanup();
            worker_finish();
            return;
        }
        io_log("subghz: BRUTE START proto=%s bits=%u from=%lu to=%lu\r\n",
               subghz_protocol_name(s_brute_proto), s_brute_bits,
               (unsigned long)s_brute_idx, (unsigned long)s_brute_end);
        int32_t frame[SUBGHZ_BRUTE_FRAME_MAX];
        for (uint32_t code = s_brute_idx;
             code <= s_brute_end && s_run; code++) {
            size_t fn = subghz_protocol_encode(s_brute_proto, code, s_brute_bits,
                                               0, frame, SUBGHZ_BRUTE_FRAME_MAX);
            if (fn == 0) { s_brute_idx = code + 1; continue; }
            esp_err_t te = ESP_OK;
            for (int r = 0; r < s_brute_repeats && s_run; r++) {
                te = subghz_tx_send(frame, fn);
                if (te != ESP_OK) break;
            }
            if (te != ESP_OK) {
                if (s_run) {
                    io_log("subghz: ERR brute-send (%s)\r\n",
                           esp_err_to_name(te));
                }
                s_brute_idx = code;    /* resume will retry this code */
                break;
            }
            s_brute_idx = code + 1;    /* resume point = next code */
            if ((code & SUBGHZ_BRUTE_PROGRESS_MASK) == 0) {
                io_log("subghz: BRUTE idx=%lu of=%lu\r\n",
                       (unsigned long)code, (unsigned long)s_brute_end);
            }
        }
        subghz_tx_cleanup();
        /* idx past the end => the whole span was sent; otherwise we stopped or
         * errored and idx is the resume point. */
        if (s_brute_idx > s_brute_end) {
            io_log("subghz: BRUTE DONE last=%lu\r\n", (unsigned long)s_brute_end);
        } else {
            io_log("subghz: BRUTE STOPPED idx=%lu\r\n", (unsigned long)s_brute_idx);
        }
        worker_finish();
        return;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        err = session_configure();   /* wake + reset + apply preset + freq */
        xSemaphoreGive(s_lock);
    }
    if (err != ESP_OK) {
        io_log("subghz: ERR worker-config (%s)\r\n", esp_err_to_name(err));
        subghz_power_down();
        worker_finish();
        return;
    }

    if (mode == SUBGHZ_MODE_READ_RAW || mode == SUBGHZ_MODE_READ) {
        const char *mname = (mode == SUBGHZ_MODE_READ) ? "READ" : "READRAW";
        /* Arm the RMT capture BEFORE entering RX so no GDO0 data is lost in the
         * gap between SRX and the receiver being ready. */
        err = subghz_capture_start(raw_capture_cb, NULL);
        if (err == ESP_OK && !s_run) {
            (void)subghz_capture_stop_and_wait();
            (void)subghz_power_down();
            worker_finish();
            return;
        }
        if (err == ESP_OK) err = cc1101_strobe(CC1101_STROBE_SRX, NULL);
        if (err == ESP_OK) {
            err = wait_for_radio_state(CC1101_MARC_STATE_RX, 5);
        }
        if (err == ESP_OK && !s_run) err = cc1101_idle();
        if (err != ESP_OK) {
            (void)cc1101_idle();
            (void)subghz_capture_stop_and_wait();
            io_log("subghz: ERR %s-start (%s)\r\n", mname, esp_err_to_name(err));
            (void)subghz_power_down();
            worker_finish();
            return;
        }
        const subghz_preset_t *preset = subghz_preset_get(s_preset);
        io_log("subghz: %s START fq=%lu preset=%s thr=%d\r\n",
               mname, (unsigned long)s_freq, preset ? preset->name : "?",
               s_rssi_threshold);
        /* Poll RSSI for RAW squelch qualification. The last above-threshold
         * sample is held briefly so RMT's idle stop and queued callback do not
         * lose the just-finished RF frame. Read mode uses protocol validation. */
        while (s_run) {
            /* If the capture task self-aborted (re-arm/overflow error), don't
             * keep spinning as a live-but-deaf session — surface it and stop. */
            if (!subghz_capture_active()) {
                io_log("subghz: ERR capture-aborted\r\n");
                break;
            }
            uint8_t raw = 0;
            if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
                s_rssi_read_errors++;
            } else {
                int r = cc1101_rssi_raw_to_dbm(raw);
                s_last_rssi = r;
                update_rssi_squelch(r);
            }
            vTaskDelay(pdMS_TO_TICKS(SUBGHZ_RSSI_POLL_MS));
        }
        (void)cc1101_idle();
        esp_err_t stop_err = subghz_capture_stop_and_wait();
        if (stop_err != ESP_OK) {
            io_log("subghz: ERR capture-stop (%s)\r\n",
                   esp_err_to_name(stop_err));
        }
        (void)subghz_power_down();
        io_log("subghz: %s STOPPED\r\n", mname);
        worker_finish();
        return;
    }

    bool spectrum = (mode == SUBGHZ_MODE_SPECTRUM);
    if (!spectrum) {
        /* Force the analyzer into a known AM / 650 kHz config (Flipper loads its
         * own analyzer presets) so RSSI, AGC and bandwidth are consistent no
         * matter which preset the user had selected. analyzer_scan then toggles
         * MDMCFG4 between the coarse and fine bandwidths. */
        (void)subghz_preset_apply(subghz_preset_get(SUBGHZ_PRESET_OOK_650));
    }
    io_log(spectrum ? "subghz: SPECTRUM START\r\n" : "subghz: ANALYZER START\r\n");
    uint32_t seq = 0;
    while (s_run) {
        if (spectrum) spectrum_sweep(seq++);
        else          analyzer_scan();
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    (void)subghz_power_down();
    io_log(spectrum ? "subghz: SPECTRUM STOPPED\r\n"
                    : "subghz: ANALYZER STOPPED\r\n");
    worker_finish();
}

bool subghz_is_busy(void)
{
    return s_mode != SUBGHZ_MODE_IDLE;
}

esp_err_t subghz_stop(void)
{
    if (!s_mode_lock) {
        if (s_worker_alive) return ESP_ERR_INVALID_STATE;
        return s_radio_fault ? ESP_FAIL : ESP_OK;
    }
    if (xSemaphoreTake(s_mode_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_worker_alive) {
        xSemaphoreGive(s_mode_lock);
        return s_radio_fault ? ESP_FAIL : ESP_OK;
    }
    s_run = false;
    bool tx_mode = s_mode == SUBGHZ_MODE_TX || s_mode == SUBGHZ_MODE_BRUTE;
    xSemaphoreGive(s_mode_lock);

    esp_err_t cancel_err = ESP_OK;
    if (tx_mode) {
        cancel_err = subghz_tx_cancel();
        if (cancel_err != ESP_OK) {
            io_log("subghz: ERR tx-cancel (%s)\r\n", esp_err_to_name(cancel_err));
        }
    }
    /* Wait comfortably longer than the worker's own shutdown: in READ_RAW it
     * runs subghz_capture_stop() (up to ~1.5 s if a slow telemetry drain delays
     * the capture task) + power-down before clearing s_worker_alive. 3 s keeps
     * a normal teardown from being misreported as a timeout. s_worker_alive is
     * volatile and cleared by the worker just before it self-deletes. */
    for (int i = 0; i < 300 && s_worker_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_worker_alive) return ESP_ERR_TIMEOUT;
    if (s_radio_fault) return ESP_FAIL;
    return cancel_err;
}

/* Create the sweep worker for `mode`. Caller MUST have stopped any previous
 * worker first (subghz_stop, which is idempotent) so there is a single place
 * that owns the stop-then-start ordering. */
static esp_err_t start_mode(subghz_mode_t mode)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mode_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_radio_fault) {
        xSemaphoreGive(s_mode_lock);
        io_log("subghz: ERR radio-state-unknown (reboot required)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_worker_alive || s_mode != SUBGHZ_MODE_IDLE) {
        xSemaphoreGive(s_mode_lock);
        io_log("subghz: ERR mode-already-active\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_mode = mode;
    s_run = true;
    s_worker_alive = true;
    if (xTaskCreate(subghz_worker, "subghz", 4096, NULL, 5, NULL) != pdPASS) {
        s_run = false;
        s_mode = SUBGHZ_MODE_IDLE;
        s_worker_alive = false;
        xSemaphoreGive(s_mode_lock);
        io_log("subghz: ERR task-create\r\n");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_mode_lock);
    return ESP_OK;
}

esp_err_t subghz_analyzer_start(void)
{
    esp_err_t stop_err = subghz_stop();   /* idempotent: no-op if nothing runs */
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    return start_mode(SUBGHZ_MODE_ANALYZER);
}

esp_err_t subghz_analyzer_stop(void)
{
    return subghz_stop();
}

esp_err_t subghz_spectrum_start(uint32_t center_hz, uint32_t span_hz, int bins)
{
    uint32_t center = center_hz ? center_hz : s_freq;
    uint32_t span = span_hz ? span_hz : SUBGHZ_SPEC_DEF_SPAN;
    if (bins <= 0) bins = SUBGHZ_SPEC_DEF_BINS;
    if (bins > SUBGHZ_SPEC_MAX_BINS) bins = SUBGHZ_SPEC_MAX_BINS;
    if (span < SUBGHZ_SPEC_MIN_SPAN) span = SUBGHZ_SPEC_MIN_SPAN;
    if (span > SUBGHZ_SPEC_MAX_SPAN) span = SUBGHZ_SPEC_MAX_SPAN;
    if (!subghz_frequency_valid(center)) {
        io_log("subghz: ERR spec-center-out-of-band %lu\r\n",
               (unsigned long)center);
        return ESP_ERR_INVALID_ARG;
    }
    /* Stop any running sweep BEFORE publishing new params, so a worker that is
     * still winding down can never read a half-updated center/span/bins
     * (subghz_stop is idempotent — a no-op when nothing is running). */
    esp_err_t stop_err = subghz_stop();
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    s_spec_center = center;
    s_spec_span = span;
    s_spec_bins = bins;
    return start_mode(SUBGHZ_MODE_SPECTRUM);
}

esp_err_t subghz_spectrum_stop(void)
{
    return subghz_stop();
}

bool subghz_spectrum_band(int index, const char **label,
                          uint32_t *lo_hz, uint32_t *hi_hz)
{
    if (index < 1 || index > SUBGHZ_SPEC_BAND_COUNT) return false;
    const subghz_spec_band_t *b = &s_spec_band_tbl[index - 1];
    if (label) *label = b->label;
    if (lo_hz) *lo_hz = b->center - b->span / 2;
    if (hi_hz) *hi_hz = b->center + b->span / 2;
    return true;
}

esp_err_t subghz_spectrum_start_band(int index, int bins)
{
    if (index < 1 || index > SUBGHZ_SPEC_BAND_COUNT) {
        io_log("subghz: ERR bad-band %d\r\n", index);
        return ESP_ERR_INVALID_ARG;
    }
    const subghz_spec_band_t *b = &s_spec_band_tbl[index - 1];
    return subghz_spectrum_start(b->center, b->span, bins);
}

void subghz_spectrum_list_bands(void)
{
    io_log("subghz: SPECBANDS n=%d\r\n", SUBGHZ_SPEC_BAND_COUNT);
    for (int i = 0; i < SUBGHZ_SPEC_BAND_COUNT; i++) {
        const subghz_spec_band_t *b = &s_spec_band_tbl[i];
        io_log("subghz: SPECBAND idx=%d label=%s lo=%lu hi=%lu\r\n",
               i + 1, b->label,
               (unsigned long)(b->center - b->span / 2),
               (unsigned long)(b->center + b->span / 2));
    }
}

void subghz_set_rssi_threshold(int dbm)
{
    s_rssi_threshold = dbm;
    io_log("subghz: RSSI_THR %d\r\n", dbm);
}

int subghz_get_rssi_threshold(void)
{
    return s_rssi_threshold;
}

/* Shared bring-up for capture-based modes (READ_RAW and READ): stop any
 * previous mode, ensure the PSRAM last-capture buffer (kept after stop so a
 * later `subghz save` can read it), reset diagnostics, and start with the
 * squelch firmly CLOSED (no stale RSSI can open it before the first poll). */
static esp_err_t start_capture_mode(subghz_mode_t mode, bool strict_raw)
{
    esp_err_t stop_err = subghz_stop();
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    if (!s_raw_last) {
        s_raw_last = heap_caps_malloc(SUBGHZ_RAW_MAX_TIMINGS * sizeof(int32_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_raw_last) {
            io_log("subghz: ERR raw-buf-oom\r\n");
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_raw_last_count = 0;
    s_raw_last_truncated = false;
    s_have_decoded = false;
    memset(&s_last_decoded, 0, sizeof(s_last_decoded));
    xSemaphoreGive(s_lock);
    s_rssi_read_errors = 0;
    s_squelch_opens = 0;
    s_raw_seq = 0;
    memset(&s_read_last_reported, 0, sizeof(s_read_last_reported));
    s_read_last_reported_tick = 0;
    s_read_have_reported = false;
    s_readraw_strict = (mode == SUBGHZ_MODE_READ_RAW) && strict_raw;
    s_raw_have_reported = false;
    s_raw_last_reported_tick = 0;
    s_last_rssi = SUBGHZ_RSSI_FLOOR;
    portENTER_CRITICAL(&s_rssi_mux);
    s_rssi_last_above_tick = 0;
    s_rssi_seen_above = false;
    portEXIT_CRITICAL(&s_rssi_mux);
    esp_err_t start_err = start_mode(mode);
    if (start_err != ESP_OK) s_readraw_strict = false;
    return start_err;
}

esp_err_t subghz_readraw_start(bool strict)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return start_capture_mode(SUBGHZ_MODE_READ_RAW, strict);
}

esp_err_t subghz_readraw_stop(void)
{
    return subghz_stop();
}

esp_err_t subghz_read_start(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    return start_capture_mode(SUBGHZ_MODE_READ, false);
}

esp_err_t subghz_read_stop(void)
{
    return subghz_stop();
}

esp_err_t subghz_brute_start(const char *proto_name, int bits, int repeats)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    subghz_proto_id_t proto = subghz_protocol_from_name(proto_name);
    if (proto == SUBGHZ_PROTO_UNKNOWN) {
        io_log("subghz: ERR brute-proto-unknown %s\r\n",
               proto_name ? proto_name : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    /* Brute force emits over the OOK (AM) TX path at the current frequency, so
     * only small-code-space OOK protocols are valid. Princeton (24-bit), Holtek
     * (40-bit) and Gate-TX (24-bit) exceed the <=20-bit cap, and Ansonic is FM —
     * emitting it over OOK would produce frames no real receiver accepts.
     * Refuse them explicitly instead of pretending. CAME, Nice FLO and Linear
     * fit (<=20 bits, OOK). */
    if (proto != SUBGHZ_PROTO_CAME && proto != SUBGHZ_PROTO_NICE_FLO &&
        proto != SUBGHZ_PROTO_LINEAR) {
        io_log("subghz: ERR brute-unsupported %s (use CAME, NiceFlo or Linear)\r\n",
               subghz_protocol_name(proto));
        return ESP_ERR_INVALID_ARG;
    }
    if (bits <= 0) bits = subghz_protocol_default_bits(proto);
    if (bits <= 0 || bits > SUBGHZ_BRUTE_MAX_BITS) {
        io_log("subghz: ERR brute-bits %d (1..%d)\r\n", bits, SUBGHZ_BRUTE_MAX_BITS);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t stop_err = subghz_stop();
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    s_brute_proto = proto;
    s_brute_bits = (uint16_t)bits;
    s_brute_idx = 0;
    s_brute_end = (uint32_t)((1UL << bits) - 1UL);   /* bits<=20 => fits u32 */
    s_brute_repeats = repeats > 0 ? repeats : SUBGHZ_BRUTE_DEF_REPEATS;
    if (s_brute_repeats > SUBGHZ_TX_MAX_REPEATS) s_brute_repeats = SUBGHZ_TX_MAX_REPEATS;
    /* Brute force is OOK fixed-code: latch the start frequency (so resume after
     * a `subghz freq` change stays on the frequency the run began on) + OOK. */
    s_brute_freq = s_freq;
    s_tx_freq = s_brute_freq;
    s_tx_preset = SUBGHZ_DEFAULT_PRESET;
    return start_mode(SUBGHZ_MODE_BRUTE);
}

esp_err_t subghz_brute_resume(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_brute_proto == SUBGHZ_PROTO_UNKNOWN || s_brute_bits == 0) {
        io_log("subghz: ERR brute-none (start one first)\r\n");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_brute_idx > s_brute_end) {
        io_log("subghz: BRUTE already complete\r\n");
        return ESP_OK;
    }
    esp_err_t stop_err = subghz_stop();
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    /* Resume on the ORIGINAL frequency, not whatever s_freq is now. */
    s_tx_freq = s_brute_freq;
    s_tx_preset = SUBGHZ_DEFAULT_PRESET;
    return start_mode(SUBGHZ_MODE_BRUTE);
}

esp_err_t subghz_brute_stop(void)
{
    return subghz_stop();
}

void subghz_readraw_stats(void)
{
    subghz_capture_stats_t st;
    subghz_capture_get_stats(&st);
    io_log("subghz: RAWSTATS sessions=%lu frames=%lu splits=%lu rx_sym=%lu trunc=%lu "
           "bufovf=%lu rearm=%lu cbmax_us=%lu squelch=%lu rssi_err=%lu "
           "last_rssi=%d\r\n",
           (unsigned long)st.sessions, (unsigned long)st.bursts,
           (unsigned long)st.frame_splits,
           (unsigned long)st.received_symbols, (unsigned long)st.truncated,
           (unsigned long)st.buffer_overflow, (unsigned long)st.rmt_restarts,
           (unsigned long)st.max_cb_latency_us, (unsigned long)s_squelch_opens,
           (unsigned long)s_rssi_read_errors, s_last_rssi);
}

esp_err_t subghz_save(const char *name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name) return ESP_ERR_INVALID_ARG;
    bool capture_active = (s_mode == SUBGHZ_MODE_READ || s_mode == SUBGHZ_MODE_READ_RAW);
    if (subghz_is_busy() && !capture_active) {
        io_log("subghz: ERR busy (stop TX/sweep before saving)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    int32_t *snapshot = heap_caps_malloc(SUBGHZ_RAW_MAX_TIMINGS * sizeof(int32_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        io_log("subghz: ERR save-oom\r\n");
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        heap_caps_free(snapshot);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_raw_last || s_raw_last_count == 0) {
        xSemaphoreGive(s_lock);
        heap_caps_free(snapshot);
        io_log("subghz: NOCAP\r\n");   /* nothing captured yet */
        return ESP_ERR_NOT_FOUND;
    }
    size_t count = s_raw_last_count;
    memcpy(snapshot, s_raw_last, count * sizeof(int32_t));
    uint32_t freq = s_raw_last_freq;
    subghz_preset_id_t preset_id = s_raw_last_preset;
    bool truncated = s_raw_last_truncated;
    bool have_decoded = s_have_decoded;
    subghz_decoded_t decoded = s_last_decoded;
    xSemaphoreGive(s_lock);

    const subghz_preset_t *p = subghz_preset_get(preset_id);
    esp_err_t err;
    if (have_decoded) {
        /* The last capture decoded to a known protocol -> save a Key File
         * (Protocol/Bit/Key/TE), like Flipper's Read -> Save. */
        err = subghz_store_save_key(name,
                                    subghz_protocol_name(decoded.proto),
                                    decoded.bits, decoded.key, decoded.te, freq,
                                    p ? p->flipper_name : NULL);
    } else {
        if (truncated) {
            io_log("subghz: WARN saving truncated capture (edges may be lost)\r\n");
        }
        err = subghz_store_save_raw(name, snapshot, count, freq,
                                    p ? p->flipper_name : NULL);
    }
    heap_caps_free(snapshot);

    if (err == ESP_ERR_INVALID_STATE) {
        io_log("subghz: ERR no-storage-target (set `subghz storage <littlefs|sd>`)\r\n");
    } else if (err == ESP_ERR_NOT_FOUND) {
        io_log("subghz: ERR sd-absent\r\n");
    }
    return err;
}

esp_err_t subghz_save_key_fields(const char *name, const char *proto_name,
                                 int bits, uint64_t key, uint32_t te,
                                 uint32_t freq, const char *preset_id)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name || !proto_name) return ESP_ERR_INVALID_ARG;
    /* Writing a .sub uses the shared SPI bus (SD) but never the radio, so it is
     * allowed while a Read/Read-RAW capture is listening (mirrors subghz_save);
     * only an active TX/sweep is refused. */
    bool capture_active =
        (s_mode == SUBGHZ_MODE_READ || s_mode == SUBGHZ_MODE_READ_RAW);
    if (subghz_is_busy() && !capture_active) {
        io_log("subghz: ERR busy (stop TX/sweep before saving)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    subghz_proto_id_t proto = subghz_protocol_from_name(proto_name);
    if (proto == SUBGHZ_PROTO_UNKNOWN) {
        io_log("subghz: ERR savekey-proto-unknown %s\r\n", proto_name);
        return ESP_ERR_INVALID_ARG;
    }
    if (bits <= 0 || bits > 64) {
        io_log("subghz: ERR savekey-bits %d\r\n", bits);
        return ESP_ERR_INVALID_ARG;
    }
    if (bits < 64 && (key >> bits) != 0) {
        io_log("subghz: ERR savekey-key-overflow bits=%d\r\n", bits);
        return ESP_ERR_INVALID_ARG;
    }
    if (freq == 0 || !subghz_frequency_valid(freq)) {
        io_log("subghz: ERR savekey-freq-invalid %lu\r\n", (unsigned long)freq);
        return ESP_ERR_INVALID_ARG;
    }
    /* Resolve the preset by its CLI token; fall back to the current preset so a
     * missing/unknown token never blocks the save. */
    const subghz_preset_t *p =
        preset_id ? subghz_preset_from_name(preset_id) : NULL;
    if (!p) p = subghz_preset_get(s_preset);
    esp_err_t err = subghz_store_save_key(name, subghz_protocol_name(proto),
                                          (uint16_t)bits, key, te, freq,
                                          p ? p->flipper_name : NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        io_log("subghz: ERR no-storage-target (set `subghz storage <littlefs|sd>`)\r\n");
    } else if (err == ESP_ERR_NOT_FOUND) {
        io_log("subghz: ERR sd-absent\r\n");
    }
    return err;
}

void subghz_info(const char *name)
{
    if (!s_ready) { io_log("subghz: ERR not-ready\r\n"); return; }
    if (!name) { io_log("usage: subghz info <name>\r\n"); return; }
    if (subghz_is_busy()) {
        io_log("subghz: ERR busy (stop before reading files)\r\n");
        return;
    }
    subghz_file_kind_t kind = subghz_store_classify(name);
    if (kind == SUBGHZ_FILE_KEY) {
        char proto[16] = {0}, preset[48] = {0};
        uint16_t bits = 0; uint64_t key = 0; uint32_t te = 0, freq = 0;
        if (subghz_store_load_key(name, proto, sizeof(proto), &bits, &key, &te,
                                  &freq, preset, sizeof(preset)) == ESP_OK) {
            io_log("subghz: INFO name=%s kind=key proto=%s bits=%u "
                   "key=0x%08lX%08lX te=%lu fq=%lu preset=%s\r\n",
                   name, proto, (unsigned)bits,
                   (unsigned long)(uint32_t)(key >> 32),
                   (unsigned long)(uint32_t)(key & 0xFFFFFFFFULL),
                   (unsigned long)te, (unsigned long)freq, preset);
        } else {
            io_log("subghz: INFO name=%s kind=key (unreadable)\r\n", name);
        }
    } else if (kind == SUBGHZ_FILE_RAW) {
        if (!s_emu_buf) {
            s_emu_buf = heap_caps_malloc(SUBGHZ_RAW_MAX_TIMINGS * sizeof(int32_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        size_t n = 0; uint32_t freq = 0; char preset[48] = {0};
        if (s_emu_buf &&
            subghz_store_load_raw(name, s_emu_buf, SUBGHZ_RAW_MAX_TIMINGS, &n,
                                  &freq, preset, sizeof(preset)) == ESP_OK) {
            io_log("subghz: INFO name=%s kind=raw n=%u fq=%lu preset=%s\r\n",
                   name, (unsigned)n, (unsigned long)freq, preset);
        } else {
            io_log("subghz: INFO name=%s kind=raw (unreadable)\r\n", name);
        }
    } else {
        io_log("subghz: INFO name=%s not-found/unreadable\r\n", name);
    }
}

esp_err_t subghz_tx_last(int repeats)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    esp_err_t stop_err = subghz_stop();
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_raw_last || s_raw_last_count == 0) {
        xSemaphoreGive(s_lock);
        io_log("subghz: NOCAP\r\n");
        return ESP_ERR_NOT_FOUND;
    }
    s_tx_src = s_raw_last;
    s_tx_count = s_raw_last_count;
    s_tx_freq = s_raw_last_freq;
    s_tx_preset = s_raw_last_preset;
    xSemaphoreGive(s_lock);
    s_tx_repeats = repeats > 0 ? repeats : SUBGHZ_TX_DEFAULT_REPEATS;
    if (s_tx_repeats > SUBGHZ_TX_MAX_REPEATS) s_tx_repeats = SUBGHZ_TX_MAX_REPEATS;
    s_tx_gap_ms = SUBGHZ_TX_GAP_MS;
    return start_mode(SUBGHZ_MODE_TX);
}

esp_err_t subghz_emulate(const char *name, int repeats)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!name) return ESP_ERR_INVALID_ARG;
    esp_err_t stop_err = subghz_stop();
    if (stop_err != ESP_OK) {
        io_log("subghz: ERR stop-previous (%s)\r\n", esp_err_to_name(stop_err));
        return stop_err;
    }
    if (!s_emu_buf) {
        s_emu_buf = heap_caps_malloc(SUBGHZ_RAW_MAX_TIMINGS * sizeof(int32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_emu_buf) { io_log("subghz: ERR emu-oom\r\n"); return ESP_ERR_NO_MEM; }
    }
    size_t n = 0;
    uint32_t freq = 0;
    char preset_name[48] = {0};
    subghz_proto_id_t emu_proto = SUBGHZ_PROTO_UNKNOWN;   /* set for KEY files */
    subghz_file_kind_t kind = subghz_store_classify(name);
    if (kind == SUBGHZ_FILE_KEY) {
        /* Protocol Key File -> re-encode the waveform from Protocol/Bit/Key/TE. */
        char proto_name[16] = {0};
        uint16_t kbits = 0;
        uint64_t kkey = 0;
        uint32_t kte = 0;
        esp_err_t e = subghz_store_load_key(name, proto_name, sizeof(proto_name),
                                            &kbits, &kkey, &kte, &freq,
                                            preset_name, sizeof(preset_name));
        if (e != ESP_OK) {
            io_log("subghz: ERR emu-key-load (%s)\r\n", esp_err_to_name(e));
            return e;
        }
        emu_proto = subghz_protocol_from_name(proto_name);
        if (emu_proto == SUBGHZ_PROTO_UNKNOWN) {
            io_log("subghz: ERR emu-proto-unsupported %s\r\n",
                   proto_name[0] ? proto_name : "(none)");
            return ESP_ERR_INVALID_STATE;
        }
        /* Reject a key whose value does not fit its declared bit width — a
         * syntactically valid but internally inconsistent file. */
        if (kbits == 0 || kbits > 64 ||
            (kbits < 64 && (kkey >> kbits) != 0)) {
            io_log("subghz: ERR emu-key-bits proto=%s bits=%u\r\n",
                   proto_name, (unsigned)kbits);
            return ESP_ERR_INVALID_ARG;
        }
        n = subghz_protocol_encode(emu_proto, kkey, kbits, kte, s_emu_buf,
                                   SUBGHZ_RAW_MAX_TIMINGS);
        if (n == 0) {
            io_log("subghz: ERR emu-encode (proto=%s bits=%u)\r\n",
                   proto_name, (unsigned)kbits);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        /* RAW file (or NONE: load_raw yields the precise error). */
        esp_err_t e = subghz_store_load_raw(name, s_emu_buf, SUBGHZ_RAW_MAX_TIMINGS,
                                            &n, &freq, preset_name,
                                            sizeof(preset_name));
        if (e != ESP_OK) {
            if (e == ESP_ERR_INVALID_STATE) {
                io_log("subghz: ERR no-storage-target\r\n");
            } else if (e == ESP_ERR_NOT_FOUND) {
                io_log("subghz: ERR emu-not-found (or sd-absent)\r\n");
            } else {
                io_log("subghz: ERR emu-load (%s)\r\n", esp_err_to_name(e));
            }
            return e;
        }
    }
    /* Require a valid in-band frequency from the file — never silently default,
     * which could transmit a 315 MHz capture on 433.92 MHz. */
    if (freq == 0 || !subghz_frequency_valid(freq)) {
        io_log("subghz: ERR emu-freq-invalid %lu\r\n", (unsigned long)freq);
        return ESP_ERR_INVALID_ARG;
    }
    /* Whitelist the preset: only presets this TX backend actually supports are
     * allowed. An unknown or custom preset is REFUSED (never silently coerced
     * to OOK), because a valid file format does not mean the radio config is
     * reproducible here. */
    const subghz_preset_t *p = subghz_preset_from_name(preset_name);
    if (!p) {
        io_log("subghz: ERR emu-preset-unsupported %s\r\n",
               preset_name[0] ? preset_name : "(none)");
        return ESP_ERR_INVALID_STATE;
    }
    /* For a Key File, the protocol's native modulation MUST match the file's
     * preset modulation. Otherwise we would key the CC1101 in the wrong mode
     * (e.g. an FM Ansonic key under an OOK preset, or an AM protocol under a
     * 2FSK preset) and transmit a frame no real receiver accepts. Refuse before
     * any transmission rather than emit a wrong-modulation burst. (A RAW file
     * has no protocol, so it just replays under its own captured preset.) */
    if (emu_proto != SUBGHZ_PROTO_UNKNOWN &&
        subghz_protocol_is_fm(emu_proto) != p->fm) {
        io_log("subghz: ERR emu-modulation-mismatch proto=%s(%s) preset=%s(%s)\r\n",
               subghz_protocol_name(emu_proto),
               subghz_protocol_is_fm(emu_proto) ? "FM" : "AM",
               p->name, p->fm ? "FM" : "AM");
        return ESP_ERR_INVALID_STATE;
    }
    s_tx_src = s_emu_buf;
    s_tx_count = n;
    s_tx_freq = freq;
    s_tx_preset = preset_id_of(p);
    s_tx_repeats = repeats > 0 ? repeats : SUBGHZ_TX_DEFAULT_REPEATS;
    if (s_tx_repeats > SUBGHZ_TX_MAX_REPEATS) s_tx_repeats = SUBGHZ_TX_MAX_REPEATS;
    s_tx_gap_ms = SUBGHZ_TX_GAP_MS;
    io_log("subghz: EMULATE name=%s n=%u fq=%lu preset=%s\r\n",
           name, (unsigned)n, (unsigned long)s_tx_freq, p ? p->name : "default");
    return start_mode(SUBGHZ_MODE_TX);
}

esp_err_t subghz_status(void)
{
    if (!s_ready) {
        io_log("subghz: ERR not-ready\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (subghz_is_busy()) {
        io_log("subghz: ERR busy (sweep active)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_radio_fault) {
        io_log("subghz: ERR radio-state-unknown (reboot required)\r\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = session_configure();
    if (err != ESP_OK) {
        io_log("subghz: ERR configure (%s)\r\n", esp_err_to_name(err));
        subghz_power_down();
        xSemaphoreGive(s_lock);
        return err;
    }

    uint8_t version = 0, partnum = 0, marc = 0;
    esp_err_t rerr = cc1101_read_status(CC1101_STATUS_VERSION, &version);
    if (rerr != ESP_OK) {
        /* A failed SPI read must not be reported as a present chip with a
         * garbage (0x00) identity — surface the read error instead. */
        io_log("subghz: ERR status-read (%s)\r\n", esp_err_to_name(rerr));
        subghz_power_down();
        xSemaphoreGive(s_lock);
        return rerr;
    }
    cc1101_read_status(CC1101_STATUS_PARTNUM, &partnum);

    /* Enter RX briefly so the RSSI reading reflects the configured band.
     * Yield (vTaskDelay) rather than busy-spin so the command task stays
     * responsive while the synth calibrates and RSSI settles. */
    cc1101_strobe(CC1101_STROBE_SRX, NULL);
    for (int i = 0; i < 5; i++) {
        uint8_t st = 0;
        if (cc1101_read_marcstate(&st) == ESP_OK && st == CC1101_MARC_STATE_RX) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    int rssi = cc1101_read_rssi_dbm();
    cc1101_read_marcstate(&marc);

    esp_err_t power_err = subghz_power_down();
    if (power_err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return power_err;
    }

    const subghz_preset_t *preset = subghz_preset_get(s_preset);
    io_log("subghz: STATUS present=1 ver=0x%02X part=0x%02X fq=%lu preset=%s "
           "rssi=%d marc=0x%02X\r\n",
           version, partnum, (unsigned long)s_freq,
           preset ? preset->name : "?", rssi, marc);

    xSemaphoreGive(s_lock);
    return ESP_OK;
}
