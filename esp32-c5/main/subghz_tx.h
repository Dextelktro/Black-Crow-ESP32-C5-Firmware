#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 * Sub-GHz OOK transmit via a GPTimer-driven GPIO toggle.
 *
 * In CC1101 asynchronous-serial OOK mode (IOCFG0=0x0D, PKTCTRL0=0x32) the MCU
 * DRIVES GDO0 and the chip keys the PA to follow it (high = carrier via
 * PATABLE[1], low = off via PATABLE[0]). We can't use an RMT TX channel (LED +
 * IR already hold both TX-capable channels), so a GPTimer re-arms per edge from
 * its on_alarm callback (a "monotonic software timer list", per the ESP-IDF
 * docs) toggling the GDO0 GPIO. Durations are signed microseconds, Flipper RAW
 * convention (+ = mark/high, - = space/low). The first element MAY be a space
 * (negative) — e.g. a protocol frame that opens with its header gap; the
 * sender applies each segment's level regardless of sign, so a leading low is
 * simply carrier-off for that duration.
 *
 * Memory: the duration list is copied into an internal-RAM buffer on begin()
 * (ISR reads it without depending on the PSRAM cache) and freed on end(); the
 * GPTimer + semaphore live only between begin() and end().
 *
 * Caller contract (done by the Sub-GHz worker): apply the OOK preset + set the
 * frequency over SPI, subghz_tx_begin(), strobe STX, subghz_tx_send() one or
 * more times, strobe SIDLE, subghz_tx_end().
 * ============================================================ */

/* Set up the GPIO (as output, low), GPTimer and completion semaphore. */
esp_err_t subghz_tx_begin(int gpio_num);

/* Transmit one frame (blocking until the whole duration list is on the air).
 * `durations` may live in PSRAM; it is copied internally. Returns
 * ESP_ERR_TIMEOUT if the frame does not complete in time. */
esp_err_t subghz_tx_send(const int32_t *durations, size_t count);

/* Tear down: delete the GPTimer + semaphore, free the buffer, return the GPIO
 * to input. Safe to call even if begin() partially failed. */
void subghz_tx_end(void);

/* Worst ISR lateness (us) observed since begin() — a self-measured jitter
 * indicator (no external instrument needed). */
uint32_t subghz_tx_max_late_us(void);

/* Count durations whose magnitude is below the schedulable minimum (they will
 * be clamped on send). Lets the caller warn that timing is not guaranteed. */
size_t subghz_tx_count_below_min(const int32_t *durations, size_t count);
