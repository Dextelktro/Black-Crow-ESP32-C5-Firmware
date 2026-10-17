#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define NRF24_REG_CONFIG       0x00
#define NRF24_REG_EN_AA        0x01
#define NRF24_REG_EN_RXADDR    0x02
#define NRF24_REG_SETUP_AW     0x03
#define NRF24_REG_SETUP_RETR   0x04
#define NRF24_REG_RF_CH        0x05
#define NRF24_REG_RF_SETUP     0x06
#define NRF24_REG_STATUS       0x07
#define NRF24_REG_OBSERVE_TX   0x08
#define NRF24_REG_RPD          0x09
#define NRF24_REG_RX_ADDR_P0   0x0A
#define NRF24_REG_RX_ADDR_P1   0x0B
#define NRF24_REG_RX_ADDR_P2   0x0C
#define NRF24_REG_RX_ADDR_P3   0x0D
#define NRF24_REG_RX_ADDR_P4   0x0E
#define NRF24_REG_RX_ADDR_P5   0x0F
#define NRF24_REG_TX_ADDR      0x10
#define NRF24_REG_RX_PW_P0     0x11
#define NRF24_REG_RX_PW_P1     0x12
#define NRF24_REG_RX_PW_P2     0x13
#define NRF24_REG_RX_PW_P3     0x14
#define NRF24_REG_RX_PW_P4     0x15
#define NRF24_REG_RX_PW_P5     0x16
#define NRF24_REG_FIFO_STATUS  0x17
#define NRF24_REG_DYNPD        0x1C
#define NRF24_REG_FEATURE      0x1D

#define NRF24_CMD_R_REGISTER         0x00
#define NRF24_CMD_W_REGISTER         0x20
#define NRF24_CMD_R_RX_PAYLOAD       0x61
#define NRF24_CMD_W_TX_PAYLOAD       0xA0
#define NRF24_CMD_FLUSH_TX           0xE1
#define NRF24_CMD_FLUSH_RX           0xE2
#define NRF24_CMD_R_RX_PL_WID        0x60
#define NRF24_CMD_W_TX_PAYLOAD_NOACK 0xB0
#define NRF24_CMD_NOP                0xFF

#define NRF24_CONFIG_MASK_RX_DR   0x40
#define NRF24_CONFIG_MASK_TX_DS   0x20
#define NRF24_CONFIG_MASK_MAX_RT  0x10
#define NRF24_CONFIG_EN_CRC       0x08
#define NRF24_CONFIG_CRCO         0x04
#define NRF24_CONFIG_PWR_UP       0x02
#define NRF24_CONFIG_PRIM_RX      0x01

#define NRF24_STATUS_RX_DR        0x40
#define NRF24_STATUS_TX_DS        0x20
#define NRF24_STATUS_MAX_RT       0x10
#define NRF24_STATUS_TX_FULL      0x01

#define NRF24_FIFO_TX_REUSE       0x40
#define NRF24_FIFO_TX_FULL        0x20
#define NRF24_FIFO_TX_EMPTY       0x10
#define NRF24_FIFO_RX_FULL        0x02
#define NRF24_FIFO_RX_EMPTY       0x01

#define NRF24_RF_PWR_0DBM         0x06
#define NRF24_RF_PWR_M6DBM        0x04
#define NRF24_RF_PWR_M12DBM       0x02
#define NRF24_RF_PWR_M18DBM       0x00
#define NRF24_RF_DR_HIGH          0x08
#define NRF24_RF_DR_LOW           0x20
#define NRF24_RF_PLL_LOCK         0x10
#define NRF24_RF_CONT_WAVE        0x80

#define NRF24_FEATURE_EN_DYN_ACK  0x01
#define NRF24_FEATURE_EN_ACK_PAY  0x02
#define NRF24_FEATURE_EN_DPL      0x04

/* Timing constants — untuk sub-modul */
#define NRF24_CH_SETTLE_US        150
#define NRF24_CE_PULSE_US         15

typedef enum {
    NRF24_RATE_250K = 0,
    NRF24_RATE_1M,
    NRF24_RATE_2M,
} nrf24_rate_t;

typedef enum {
    NRF24_PA_MIN = 0,
    NRF24_PA_LOW,
    NRF24_PA_HIGH,
    NRF24_PA_MAX,
} nrf24_pa_t;

typedef enum {
    NRF24_AW_3 = 3,
    NRF24_AW_4 = 4,
    NRF24_AW_5 = 5,
} nrf24_aw_t;

typedef struct {
    int pin_sck;
    int pin_miso;
    int pin_mosi;
    int pin_csn;
    int pin_ce;
    int pin_irq;
    uint32_t spi_clock_hz;
} nrf24_pins_t;

esp_err_t nrf24_init(const nrf24_pins_t *pins);
esp_err_t nrf24_deinit(void);
bool      nrf24_is_initialized(void);
bool      nrf24_is_present(void);
esp_err_t nrf24_selftest(void);

uint8_t   nrf24_read_reg(uint8_t reg);
esp_err_t nrf24_write_reg(uint8_t reg, uint8_t val);
esp_err_t nrf24_read_buf(uint8_t reg, uint8_t *buf, size_t len);
esp_err_t nrf24_write_buf(uint8_t reg, const uint8_t *buf, size_t len);

uint8_t nrf24_status(void);
uint8_t nrf24_fifo_status(void);
void    nrf24_flush_tx(void);
void    nrf24_flush_rx(void);
void    nrf24_clear_irq(void);

esp_err_t     nrf24_set_channel(uint8_t ch);
uint8_t       nrf24_get_channel(void);
esp_err_t     nrf24_set_data_rate(nrf24_rate_t r);
nrf24_rate_t  nrf24_get_data_rate(void);
esp_err_t     nrf24_set_pa_level(nrf24_pa_t pa);
nrf24_pa_t    nrf24_get_pa_level(void);
esp_err_t     nrf24_set_addr_width(nrf24_aw_t aw);
nrf24_aw_t    nrf24_get_addr_width(void);
esp_err_t     nrf24_set_tx_address(const uint8_t *addr, size_t len);
esp_err_t     nrf24_set_rx_address(uint8_t pipe, const uint8_t *addr, size_t len);
esp_err_t     nrf24_set_rx_payload_size(uint8_t pipe, uint8_t size);
esp_err_t     nrf24_enable_pipe(uint8_t pipe, bool en);
esp_err_t     nrf24_enable_auto_ack(uint8_t pipe, bool en);
esp_err_t     nrf24_enable_crc(bool en, bool two_bytes);
esp_err_t     nrf24_set_retries(uint8_t delay_250us, uint8_t count);

esp_err_t     nrf24_enable_dynamic_ack(bool en);
esp_err_t     nrf24_enable_dynamic_payload(bool en);
esp_err_t     nrf24_enable_ack_payload(bool en);

esp_err_t nrf24_power_up(void);
esp_err_t nrf24_power_down(void);
bool      nrf24_is_powered(void);
esp_err_t nrf24_enter_tx(void);
esp_err_t nrf24_enter_rx(void);
esp_err_t nrf24_standby(void);
bool      nrf24_is_rx_mode(void);

esp_err_t nrf24_tx(const uint8_t *data, size_t len);
esp_err_t nrf24_tx_no_ack(const uint8_t *data, size_t len);
esp_err_t nrf24_tx_wait(const uint8_t *data, size_t len, uint32_t timeout_us);

int       nrf24_rx(uint8_t *data, size_t max_len);
bool      nrf24_rx_available(void);
bool      nrf24_get_rpd(void);
esp_err_t nrf24_sample_rpd_window(bool *detected);

esp_err_t nrf24_start_carrier(void);
esp_err_t nrf24_stop_carrier(void);
bool      nrf24_is_carrier_on(void);

void nrf24_dump_status(void);

esp_err_t nrf24_enter_promiscuous(void);
esp_err_t nrf24_exit_promiscuous(void);
bool      nrf24_is_promiscuous(void);

bool nrf24_radio_busy(void);
bool nrf24_radio_claim(const char *owner);   /* FIX H1: fail-fast, return false kalau busy */
void nrf24_radio_release(void);
