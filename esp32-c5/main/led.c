#include "led.h"

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

#include "attack.h"
#include "transport_ble.h"

/* ESP32-C5-DevKitC-1: addressable RGB LED driven by GPIO27. */
#define LED_GPIO   27
#define TICK_MS    25

static const char *TAG = "led";
static led_strip_handle_t s_led_strip;

/* ---- user-controllable state ---- */
static volatile bool         s_auto = true;
static volatile led_effect_t s_effect = LED_EFFECT_SOLID;
static volatile uint8_t      s_r = 0, s_g = 255, s_b = 128;
static volatile uint8_t      s_brightness = 64; /* conservative default */

static uint8_t scale8(uint8_t v, uint8_t s)
{
    return (uint8_t)(((uint16_t)v * (uint16_t)s) / 255);
}

static void led_put(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t br = s_brightness;
    esp_err_t err = led_strip_set_pixel(s_led_strip, 0,
                                        scale8(r, br), scale8(g, br),
                                        scale8(b, br));
    if (err == ESP_OK) err = led_strip_refresh(s_led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED update failed: %s", esp_err_to_name(err));
    }
}

/* Integer HSV->RGB, all args/results 0-255. */
static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 43;
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((v * (255 - s)) / 255);
    uint8_t q = (uint8_t)((v * (255 - ((s * rem) / 255))) / 255);
    uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) / 255))) / 255);
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/* triangle 0..255..0 over `period` ms */
static uint8_t triangle(uint32_t t, uint32_t period)
{
    uint32_t pos = t % period;
    uint32_t half = period / 2;
    if (pos < half) return (uint8_t)(pos * 255 / half);
    return (uint8_t)((period - pos) * 255 / half);
}

static void render_auto(uint32_t t)
{
    uint8_t r = 0, g = 0, b = 0;
    if (attack_is_running()) {
        /* fast red blink */
        if ((t / 120) % 2 == 0) r = 255;
    } else if (transport_ble_connected_count() > 0) {
        b = triangle(t, 2600); /* gentle blue breathe */
    } else {
        g = triangle(t, 2600); /* gentle green breathe */
    }
    led_put(r, g, b);
}

static void render_manual(uint32_t t)
{
    uint8_t r = s_r, g = s_g, b = s_b;
    switch (s_effect) {
        case LED_EFFECT_OFF:
            led_put(0, 0, 0);
            return;
        case LED_EFFECT_SOLID:
            led_put(r, g, b);
            return;
        case LED_EFFECT_BLINK:
            if ((t / 500) % 2 == 0) led_put(r, g, b);
            else                    led_put(0, 0, 0);
            return;
        case LED_EFFECT_BREATHE: {
            uint8_t lvl = triangle(t, 2400);
            led_put(scale8(r, lvl), scale8(g, lvl), scale8(b, lvl));
            return;
        }
        case LED_EFFECT_STROBE:
            if ((t % 100) < 40) led_put(r, g, b);
            else                led_put(0, 0, 0);
            return;
        case LED_EFFECT_RAINBOW: {
            uint8_t h = (uint8_t)((t / 12) & 0xFF); /* ~3s full cycle */
            uint8_t rr, gg, bb;
            hsv_to_rgb(h, 255, 255, &rr, &gg, &bb);
            led_put(rr, gg, bb);
            return;
        }
    }
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t t = 0;
    while (1) {
        if (s_auto) render_auto(t);
        else        render_manual(t);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        t += TICK_MS;
    }
}

void led_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config,
                                             &s_led_strip));
    ESP_ERROR_CHECK(led_strip_clear(s_led_strip));

    if (xTaskCreate(led_task, "led", 2560, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create LED task");
    }
}

void led_set_auto(void)
{
    s_auto = true;
}

void led_set_effect(led_effect_t effect, uint8_t r, uint8_t g, uint8_t b)
{
    s_effect = effect;
    s_r = r;
    s_g = g;
    s_b = b;
    s_auto = false;
}

void led_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

bool led_is_auto(void)
{
    return s_auto;
}
