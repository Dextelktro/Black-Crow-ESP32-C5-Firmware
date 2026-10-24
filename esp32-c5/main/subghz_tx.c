#include "subghz_tx.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "io.h"

#define SUBGHZ_TX_RESOLUTION_HZ 1000000     /* 1 tick = 1 us */
#define SUBGHZ_TX_MAX_TIMINGS   1024
#define SUBGHZ_TX_WAIT_MS       6000        /* bound a stalled frame */
/* ESP-IDF: a GPTimer alarm period below 5 us is "not recommended"; pulses
 * shorter than this cannot be reproduced with timing guarantees on this
 * backend. We clamp to it (and the caller warns) rather than feed the timer an
 * unschedulable sub-5us alarm. */
#define SUBGHZ_TX_MIN_PULSE_US  5

/* The callback stays in flash by default (zero IRAM, matching the project's
 * IRAM discipline and the IR front-end). While Sub-GHz TX itself never writes
 * flash, a flash ERASE from ANOTHER task during a transmit disables the cache
 * for the entire erase (tens of ms), stalling this flash-resident ISR and
 * HOLDING the in-progress edge for that window — a gross distortion of the
 * frame, not mere jitter. The default repeats give some redundancy, but for
 * guaranteed edge integrity under system load enable
 * CONFIG_GPTIMER_ISR_CACHE_SAFE + CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM: the
 * callback is IRAM_ATTR-guarded below and its data / alarm-config are already
 * internal / static, ready for that. */
#if CONFIG_GPTIMER_ISR_CACHE_SAFE
#define SUBGHZ_TX_CB_ATTR IRAM_ATTR
#else
#define SUBGHZ_TX_CB_ATTR
#endif

static gptimer_handle_t  s_timer;
static SemaphoreHandle_t s_done;
static int32_t          *s_buf;       /* internal RAM working copy */
static int               s_gpio = -1;
static size_t            s_n;
static volatile size_t   s_idx;       /* next segment index to apply */
static volatile bool     s_tx_err;    /* ISR flagged an alarm-scheduling failure */
static volatile uint32_t s_tx_max_late_us;  /* diagnostic: worst ISR lateness */

/* on_alarm runs in ISR context. It is intentionally NOT in IRAM: Sub-GHz TX
 * never writes flash concurrently (single active mode), so the cache stays
 * enabled and the flash-resident driver ISR + this callback + gptimer control
 * functions all run normally. gpio_set_level is IRAM-safe. */
static bool SUBGHZ_TX_CB_ATTR tx_on_alarm(gptimer_handle_t timer,
                                          const gptimer_alarm_event_data_t *edata,
                                          void *ctx)
{
    (void)ctx;
    /* Static (not stack/rodata) so it is valid when gptimer control functions
     * run from IRAM with the cache disabled. The GPTimer ISR is not reentrant,
     * so a single shared instance is safe. */
    static gptimer_alarm_config_t ac;

    /* Diagnostic: how far past the scheduled target this alarm actually ran
     * (ISR latency). Lets jitter be observed without external instruments. */
    uint64_t late = edata->count_value - edata->alarm_value;
    if (late > s_tx_max_late_us) s_tx_max_late_us = (uint32_t)late;

    size_t i = s_idx;
    BaseType_t hp = pdFALSE;
    if (i >= s_n) {
        gpio_set_level(s_gpio, 0);               /* carrier off: frame done */
        gptimer_set_alarm_action(timer, NULL);   /* disable any further alarm */
        xSemaphoreGiveFromISR(s_done, &hp);
        return hp == pdTRUE;
    }
    int32_t d = s_buf[i];
    gpio_set_level(s_gpio, d > 0 ? 1 : 0);
    s_idx = i + 1;
    uint32_t ticks = (uint32_t)(d > 0 ? d : -d);
    ac.alarm_count = edata->alarm_value + ticks;   /* 64-bit base: no overflow */
    ac.reload_count = 0;
    ac.flags.auto_reload_on_alarm = false;
    if (gptimer_set_alarm_action(timer, &ac) != ESP_OK) {
        /* Can't schedule the next edge: fail safe (carrier off) and wake the
         * sender with an error rather than emit a distorted waveform. */
        s_tx_err = true;
        gpio_set_level(s_gpio, 0);
        xSemaphoreGiveFromISR(s_done, &hp);
        return hp == pdTRUE;
    }
    return false;
}

static void tx_cleanup(void)
{
    if (s_timer) {
        gptimer_disable(s_timer);
        gptimer_del_timer(s_timer);
        s_timer = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
}

esp_err_t subghz_tx_begin(int gpio_num)
{
    if (s_timer || s_buf || s_done) return ESP_ERR_INVALID_STATE;
    s_gpio = gpio_num;
    s_tx_err = false;
    s_tx_max_late_us = 0;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    gpio_set_level(gpio_num, 0);

    s_buf = heap_caps_malloc(SUBGHZ_TX_MAX_TIMINGS * sizeof(int32_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_done = xSemaphoreCreateBinary();
    if (!s_buf || !s_done) { tx_cleanup(); return ESP_ERR_NO_MEM; }

    gptimer_config_t tc = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = SUBGHZ_TX_RESOLUTION_HZ,
    };
    err = gptimer_new_timer(&tc, &s_timer);
    if (err != ESP_OK) { io_log("subghz: tx timer alloc failed (%d)\r\n", (int)err); tx_cleanup(); return err; }

    gptimer_event_callbacks_t cbs = { .on_alarm = tx_on_alarm };
    err = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (err == ESP_OK) err = gptimer_enable(s_timer);
    if (err != ESP_OK) { tx_cleanup(); return err; }
    return ESP_OK;
}

esp_err_t subghz_tx_send(const int32_t *durations, size_t count)
{
    if (!s_timer || !s_buf || !s_done) return ESP_ERR_INVALID_STATE;
    if (!durations || count == 0) return ESP_ERR_INVALID_ARG;
    if (count > SUBGHZ_TX_MAX_TIMINGS) count = SUBGHZ_TX_MAX_TIMINGS;

    /* Copy into the internal buffer, clamping any sub-minimum magnitude up to
     * the schedulable floor (the caller warns when this happens, so it is never
     * a SILENT rounding). Sign (carrier on/off) is preserved exactly. */
    for (size_t i = 0; i < count; i++) {
        int32_t d = durations[i];
        int64_t mag = d;
        if (mag < 0) mag = -mag;          /* int64: safe even for INT32_MIN */
        if (mag < SUBGHZ_TX_MIN_PULSE_US) mag = SUBGHZ_TX_MIN_PULSE_US;
        s_buf[i] = d >= 0 ? (int32_t)mag : (int32_t)(-mag);
    }
    s_n = count;
    s_idx = 1;                       /* segment 0 applied below */
    s_tx_err = false;
    xSemaphoreTake(s_done, 0);       /* clear any stale signal */

    /* Apply the first segment and schedule its end; the ISR walks the rest. */
    gpio_set_level(s_gpio, s_buf[0] > 0 ? 1 : 0);
    gptimer_set_raw_count(s_timer, 0);
    uint32_t t0 = (uint32_t)(s_buf[0] > 0 ? s_buf[0] : -s_buf[0]);
    gptimer_alarm_config_t ac = {
        .alarm_count = t0,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    esp_err_t err = gptimer_set_alarm_action(s_timer, &ac);
    if (err == ESP_OK) err = gptimer_start(s_timer);
    if (err != ESP_OK) {
        gpio_set_level(s_gpio, 0);
        return err;
    }

    BaseType_t ok = xSemaphoreTake(s_done, pdMS_TO_TICKS(SUBGHZ_TX_WAIT_MS));
    gptimer_stop(s_timer);
    gpio_set_level(s_gpio, 0);       /* ensure carrier off on any exit */
    if (ok != pdTRUE) return ESP_ERR_TIMEOUT;
    return s_tx_err ? ESP_FAIL : ESP_OK;
}

uint32_t subghz_tx_max_late_us(void)
{
    return s_tx_max_late_us;
}

size_t subghz_tx_count_below_min(const int32_t *durations, size_t count)
{
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        int64_t mag = durations[i];
        if (mag < 0) mag = -mag;          /* int64: safe even for INT32_MIN */
        if (mag < SUBGHZ_TX_MIN_PULSE_US) n++;
    }
    return n;
}

void subghz_tx_end(void)
{
    tx_cleanup();
    if (s_gpio >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        s_gpio = -1;
    }
}
