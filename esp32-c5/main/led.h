/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* On-board status LED (pin/type per board_config.h):
 *   DevKit — addressable WS2812 RGB on GPIO27.
 *   XIAO   — plain single-colour USER_LED on GPIO27 (LEDC PWM); colours
 *            collapse to intensity, so effects still convey state.
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
