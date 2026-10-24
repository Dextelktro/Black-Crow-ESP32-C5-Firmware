#include "cc1101.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cc1101_regs.h"
#include "spi_shared_bus.h"

/* ============================================================
 * CC1101 low-level driver.
 *
 * Shares SPI2 with the SD card and nRF24 (see spi_shared_bus.c). Every
 * transaction goes through select_device(), which enforces the shared-bus
 * radio-access gate and waits for the chip's SO/MISO "ready" low before
 * clocking data — so a transfer never races SD ownership of the bus.
 *
 * The chip is left powered down (SPWD) whenever idle; the Sub-GHz feature
 * wakes it on demand and returns it to sleep when it stops.
 * ============================================================ */

#define CC1101_CS            GPIO_NUM_23
#define CC1101_GDO0          GPIO_NUM_24
#define CC1101_MISO          GPIO_NUM_2   /* SO ready indicator while CS low */
#define CC1101_SPI_CLOCK_HZ  2000000
#define CC1101_READY_TIMEOUT_US 1000
#define CC1101_RSSI_OFFSET   74           /* dBm offset (datasheet, 433 MHz) */
#define CC1101_RESET_TIMEOUT_MS 10

static spi_device_handle_t s_spi;
static bool s_present;
static bool s_sleeping;
static uint32_t s_freq_hz;

/* ---------------------------------------------------------------- */

static esp_err_t select_device(void)
{
    if (!spi_shared_bus_radio_access_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        return err;
    }
    if (!spi_shared_bus_radio_access_allowed()) {
        spi_device_release_bus(s_spi);
        return ESP_ERR_INVALID_STATE;
    }

    gpio_set_level(CC1101_CS, 0);
    for (uint32_t waited = 0; waited < CC1101_READY_TIMEOUT_US; waited++) {
        if (gpio_get_level(CC1101_MISO) == 0) {
            return ESP_OK;
        }
        esp_rom_delay_us(1);
    }

    gpio_set_level(CC1101_CS, 1);
    spi_device_release_bus(s_spi);
    return ESP_ERR_TIMEOUT;
}

static void deselect_device(void)
{
    gpio_set_level(CC1101_CS, 1);
    spi_device_release_bus(s_spi);
}

/* One full-duplex transfer. tx is required; rx may be NULL for a write.
 * Buffers are caller-owned (small, internal-RAM / stack) — the driver never
 * clocks more than a PA table (9) or a FIFO burst, so no DMA alignment dance
 * is needed here. */
static esp_err_t cc1101_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_spi || !tx || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = select_device();
    if (err != ESP_OK) {
        return err;
    }
    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    err = spi_device_polling_transmit(s_spi, &transaction);
    deselect_device();
    return err;
}

/* ---------------- register / strobe primitives ------------------ */

esp_err_t cc1101_strobe(uint8_t strobe, uint8_t *status_out)
{
    uint8_t tx = strobe;
    uint8_t rx = 0;
    esp_err_t err = cc1101_xfer(&tx, &rx, 1);
    if (err == ESP_OK && status_out) *status_out = rx;
    return err;
}

esp_err_t cc1101_write_reg(uint8_t addr, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(addr | CC1101_WRITE_SINGLE), value};
    return cc1101_xfer(tx, NULL, sizeof(tx));
}

esp_err_t cc1101_read_reg(uint8_t addr, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(addr | CC1101_READ_SINGLE), 0xFF};
    uint8_t rx[2] = {0};
    esp_err_t err = cc1101_xfer(tx, rx, sizeof(tx));
    if (err == ESP_OK && value) *value = rx[1];
    return err;
}

/* Status registers (0x30-0x3D) must be read with the burst bit set, or the
 * opcode is interpreted as a command strobe instead. Single read: the CC1101
 * read-collision de-glitch (read-until-two-agree) is applied ONLY where the
 * value is stable between updates (cc1101_read_marcstate). RSSI is inherently
 * noisy sample-to-sample, so de-glitching it would never converge and would
 * just quadruple the SPI cost for no benefit. */
esp_err_t cc1101_read_status(uint8_t addr, uint8_t *value)
{
    uint8_t tx[2] = {(uint8_t)(addr | CC1101_READ_BURST), 0xFF};
    uint8_t rx[2] = {0};
    esp_err_t err = cc1101_xfer(tx, rx, sizeof(tx));
    if (err == ESP_OK && value) *value = rx[1];
    return err;
}

esp_err_t cc1101_write_burst(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 64) return ESP_ERR_INVALID_ARG;
    uint8_t tx[65];
    tx[0] = (uint8_t)(addr | CC1101_WRITE_BURST);
    memcpy(&tx[1], data, len);
    return cc1101_xfer(tx, NULL, len + 1);
}

esp_err_t cc1101_write_patable(const uint8_t pa[8])
{
    return cc1101_write_burst(CC1101_PATABLE, pa, 8);
}

/* ---------------------- higher-level ---------------------------- */

esp_err_t cc1101_idle(void)
{
    return cc1101_strobe(CC1101_STROBE_SIDLE, NULL);
}

esp_err_t cc1101_flush_rx(void)
{
    return cc1101_strobe(CC1101_STROBE_SFRX, NULL);
}

esp_err_t cc1101_flush_tx(void)
{
    return cc1101_strobe(CC1101_STROBE_SFTX, NULL);
}

esp_err_t cc1101_reset(void)
{
    if (!s_spi || !s_present) return ESP_ERR_INVALID_STATE;
    esp_err_t err = cc1101_strobe(CC1101_STROBE_SRES, NULL);
    if (err != ESP_OK) return err;
    /* SRES returns the chip to IDLE; wait for MARCSTATE to settle. Yield with
     * vTaskDelay rather than a busy esp_rom_delay_us: this runs under the
     * Sub-GHz lock in a priority-5 task, so a non-responding chip must not spin
     * the CPU (starving the idle task / watchdog) for the whole timeout. */
    for (int i = 0; i < CC1101_RESET_TIMEOUT_MS; i++) {
        uint8_t marc = 0xFF;
        if (cc1101_read_status(CC1101_STATUS_MARCSTATE, &marc) == ESP_OK &&
            (marc & 0x1F) == CC1101_MARC_STATE_IDLE) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t cc1101_set_frequency(uint32_t hz)
{
    if (!s_spi || !s_present) return ESP_ERR_INVALID_STATE;
    /* Must be in IDLE to reprogram the synthesizer cleanly. */
    esp_err_t err = cc1101_idle();
    if (err != ESP_OK) return err;

    /* FREQ[23:0] = f_carrier * 2^16 / f_xosc. 64-bit math avoids overflow. */
    uint32_t word = (uint32_t)(((uint64_t)hz << 16) / CC1101_XOSC_HZ);
    err = cc1101_write_reg(CC1101_FREQ2, (uint8_t)((word >> 16) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_FREQ1, (uint8_t)((word >> 8) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_FREQ0, (uint8_t)(word & 0xFF));
    if (err == ESP_OK) s_freq_hz = hz;
    return err;
}

uint32_t cc1101_get_frequency(void)
{
    return s_freq_hz;
}

/* Pure RSSI register -> dBm conversion (datasheet: 2's-complement, /2, minus a
 * band/data-rate dependent offset). Exposed so callers that need to tell a read
 * FAILURE apart from a genuine weak reading can read the status register
 * themselves and convert only on success — a failed read must never look like a
 * strong signal. NOTE: this is raw RSSI, NOT the CC1101 carrier-sense status
 * (which additionally depends on AGC threshold + hysteresis); the two are not
 * interchangeable. */
int cc1101_rssi_raw_to_dbm(uint8_t raw)
{
    int rssi = (raw >= 128) ? ((int)raw - 256) : (int)raw;
    return (rssi / 2) - CC1101_RSSI_OFFSET;
}

int cc1101_read_rssi_dbm(void)
{
    uint8_t raw = 0;
    if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
        return -128;  /* read failed -> report the floor, never a strong value */
    }
    return cc1101_rssi_raw_to_dbm(raw);
}

/* MARCSTATE is where the SPI read-collision hazard matters (it can glitch
 * during a state transition) and where "read until two agree" is valid, since
 * the value is stable except during transitions. Bounded to 4 attempts. */
esp_err_t cc1101_read_marcstate(uint8_t *state)
{
    uint8_t prev = 0;
    bool have_prev = false;
    for (int i = 0; i < 4; i++) {
        uint8_t marc = 0;
        esp_err_t err = cc1101_read_status(CC1101_STATUS_MARCSTATE, &marc);
        if (err != ESP_OK) return err;
        marc &= 0x1F;
        if (have_prev && marc == prev) {
            if (state) *state = marc;
            return ESP_OK;
        }
        prev = marc;
        have_prev = true;
    }
    if (state) *state = prev;
    return ESP_OK;
}

/* ---------------------- lifecycle ------------------------------- */

static esp_err_t read_version(uint8_t *version)
{
    return cc1101_read_status(CC1101_STATUS_VERSION, version);
}

esp_err_t cc1101_init(void)
{
    if (s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!spi_shared_bus_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t gdo_config = {
        .pin_bit_mask = 1ULL << CC1101_GDO0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gdo_config);
    if (err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = CC1101_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(spi_shared_bus_host(), &device_config, &s_spi);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t version = 0;
    err = read_version(&version);
    if (err != ESP_OK || version == 0x00 || version == 0xFF) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    s_present = true;
    s_freq_hz = 0;
    err = cc1101_sleep();
    if (err != ESP_OK) {
        s_present = false;
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t cc1101_sleep(void)
{
    if (!s_spi || !s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sleeping) {
        return ESP_OK;
    }

    esp_err_t err = cc1101_strobe(CC1101_STROBE_SIDLE, NULL);
    if (err == ESP_OK) {
        err = cc1101_strobe(CC1101_STROBE_SPWD, NULL);
    }
    if (err == ESP_OK) {
        s_sleeping = true;
    }
    return err;
}

esp_err_t cc1101_wake(void)
{
    if (!s_spi || !s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_sleeping) {
        return ESP_OK;
    }

    /* The SIDLE strobe's own select_device() pulls CSn low and waits for SO
     * ready, which exits SPWD; the strobe then leaves the chip in IDLE. Mark
     * the driver awake only once that strobe succeeds — otherwise a failure
     * mid-wake would desync state and a later cc1101_sleep() would early-return
     * (thinking the chip is already powered down) and leave it drawing current. */
    esp_err_t err = cc1101_strobe(CC1101_STROBE_SIDLE, NULL);
    if (err == ESP_OK) {
        s_sleeping = false;
    }
    return err;
}

esp_err_t cc1101_deinit(void)
{
    if (!s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_present ? cc1101_sleep() : ESP_OK;
    esp_err_t remove_err = spi_bus_remove_device(s_spi);
    s_spi = NULL;
    s_present = false;
    s_sleeping = false;
    return err != ESP_OK ? err : remove_err;
}

bool cc1101_is_present(void)
{
    return s_present;
}

bool cc1101_is_sleeping(void)
{
    return s_sleeping;
}
