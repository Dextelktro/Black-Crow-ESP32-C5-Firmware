#pragma once

#include <stdint.h>
#include <stdbool.h>

/* On-board WS2812 RGB LED (ESP32-C5-DevKitC-1: GPIO27).
 *
 * Default "auto" mode reflects device state:
 *   GREEN = idle, BLUE = BLE client connected, RED = attack running.
 * The app can override with a user color + effect via led_set_effect(),
 * and restore the indicator with led_set_auto(). */

typedef enum {
    LED_EFFECT_OFF = 0,
    LED_EFFECT_SOLID,
    LED_EFFECT_BLINK,
    LED_EFFECT_BREATHE,
    LED_EFFECT_STROBE,
    LED_EFFECT_RAINBOW,
} led_effect_t;

void led_init(void);
void led_set_auto(void);
void led_set_effect(led_effect_t effect, uint8_t r, uint8_t g, uint8_t b);
void led_set_brightness(uint8_t brightness); /* 0-255 master scale */
bool led_is_auto(void);
