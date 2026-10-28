#pragma once

/* ============================================================
 * Board pin map — single source of truth for every peripheral
 * GPIO used by the firmware.
 *
 * Two boards are supported, selected at build time by the
 * Kconfig choice in main/Kconfig.projbuild:
 *
 *   CONFIG_BOARD_ESP32C5_DEVKIT (default) — ESP32-C5-DevKitC-1
 *       16 MB flash / 8 MB PSRAM, addressable WS2812 status LED,
 *       spare UART pins for the ESP-flasher feature.
 *
 *   CONFIG_BOARD_XIAO_ESP32C5 — Seeed Studio XIAO ESP32-C5
 *       8 MB flash / 8 MB PSRAM, 11 exposed pads (D0..D10),
 *       plain single-colour USER_LED on GPIO27, no spare pin for
 *       a flasher UART.
 *
 * The DevKit (#else) branch carries the EXACT values the firmware
 * shipped with, so selecting the DevKit target produces identical
 * behaviour. Only add/override pins here — never scatter raw GPIO
 * numbers back into the driver files.
 * ============================================================ */

#include "driver/gpio.h"

#if defined(CONFIG_BOARD_XIAO_ESP32C5)

/* ---- Seeed Studio XIAO ESP32-C5 (8 MB flash / 8 MB PSRAM) ----
 * Pad -> GPIO per the Seeed wiki pinout. All 11 D-pads are used. */
#define BOARD_NAME                   "XIAO-ESP32C5"

/* Shared SPI bus (native SPI pads D8/D9/D10). */
#define BOARD_SPI_SCK_GPIO           GPIO_NUM_8   /* D8  */
#define BOARD_SPI_MISO_GPIO          GPIO_NUM_9   /* D9  */
#define BOARD_SPI_MOSI_GPIO          GPIO_NUM_10  /* D10 */

/* SD card (SPI mode). */
#define BOARD_SD_CS_GPIO             GPIO_NUM_0   /* D1 */
#define BOARD_SD_CD_GPIO             GPIO_NUM_7   /* D3 (card-detect) */

/* nRF24L01+. */
#define BOARD_NRF24_CSN_GPIO         GPIO_NUM_11  /* D6 */
#define BOARD_NRF24_CE_GPIO          GPIO_NUM_12  /* D7 */

/* CC1101 Sub-GHz. */
#define BOARD_CC1101_CS_GPIO         GPIO_NUM_23  /* D4 */
#define BOARD_CC1101_GDO0_GPIO       GPIO_NUM_24  /* D5 */

/* Infrared (RMT). */
#define BOARD_IR_TX_GPIO             GPIO_NUM_1   /* D0 */
#define BOARD_IR_RX_GPIO             GPIO_NUM_25  /* D2 */

/* Status LED: onboard USER_LED (plain single-colour, NOT WS2812). */
#define BOARD_STATUS_LED_GPIO        GPIO_NUM_27
#define BOARD_HAS_WS2812             0
/* USER_LED polarity is not documented by Seeed; assume active-high.
 * Flip to 1 if the LED turns out to be active-low on hardware. */
#define BOARD_STATUS_LED_ACTIVE_LOW  0

/* ESP-flasher (flash other ESP chips over UART1): no spare pins on the
 * XIAO, so BOARD_HAS_ESP_FLASHER is 0 and the flasher entry points bail
 * out with ESP_ERR_NOT_SUPPORTED before configuring any pin (see
 * esp_flasher.c). The pin numbers below are INERT PLACEHOLDERS — they are
 * never driven at runtime; they only exist so the compiled-but-unreachable
 * flasher code has valid (non-negative) GPIO constants. Do NOT wire to
 * these pads expecting the flasher to work. */
#define BOARD_HAS_ESP_FLASHER        0
#define BOARD_FLASHER_UART_TX_GPIO   GPIO_NUM_25  /* inert placeholder */
#define BOARD_FLASHER_UART_RX_GPIO   GPIO_NUM_3   /* inert placeholder */
#define BOARD_FLASHER_BOOT_GPIO      GPIO_NUM_23  /* inert placeholder */

#else  /* -------- ESP32-C5-DevKitC-1 (ORIGINAL, UNCHANGED) -------- */

#define BOARD_NAME                   "ESP32C5-DevKitC"

/* Shared SPI bus. */
#define BOARD_SPI_SCK_GPIO           GPIO_NUM_6
#define BOARD_SPI_MISO_GPIO          GPIO_NUM_2
#define BOARD_SPI_MOSI_GPIO          GPIO_NUM_7

/* SD card (SPI mode). */
#define BOARD_SD_CS_GPIO             GPIO_NUM_1
#define BOARD_SD_CD_GPIO             GPIO_NUM_0

/* nRF24L01+. */
#define BOARD_NRF24_CSN_GPIO         GPIO_NUM_9
#define BOARD_NRF24_CE_GPIO          GPIO_NUM_8

/* CC1101 Sub-GHz. */
#define BOARD_CC1101_CS_GPIO         GPIO_NUM_10
#define BOARD_CC1101_GDO0_GPIO       GPIO_NUM_26

/* Infrared (RMT). */
#define BOARD_IR_TX_GPIO             GPIO_NUM_4
#define BOARD_IR_RX_GPIO             GPIO_NUM_5

/* Status LED: onboard addressable WS2812 RGB. */
#define BOARD_STATUS_LED_GPIO        GPIO_NUM_27
#define BOARD_HAS_WS2812             1
#define BOARD_STATUS_LED_ACTIVE_LOW  0

/* ESP-flasher over UART1. */
#define BOARD_HAS_ESP_FLASHER        1
#define BOARD_FLASHER_UART_TX_GPIO   GPIO_NUM_25
#define BOARD_FLASHER_UART_RX_GPIO   GPIO_NUM_3
#define BOARD_FLASHER_BOOT_GPIO      GPIO_NUM_23

#endif

/* CC1101 drives the shared MISO low as a "ready" indicator while its
 * CS is asserted, so the poll pin is always the shared-bus MISO. */
#define BOARD_CC1101_MISO_GPIO       BOARD_SPI_MISO_GPIO
