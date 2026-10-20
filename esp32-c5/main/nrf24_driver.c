#include "nrf24_driver.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "spi_shared_bus.h"

static const char *TAG = "nrf24";

#define NRF24_SPI_HOST         SPI2_HOST
#define NRF24_MAX_PAYLOAD      32
#define NRF24_SPI_CLOCK_MAX_HZ 10000000
#define NRF24_POWERUP_DELAY_MS 5
#define NRF24_TX_DEFAULT_TIMEOUT_US  2000

typedef struct {
    bool initialized;
    bool present;
    bool bus_owned;
    spi_device_handle_t spi;
    SemaphoreHandle_t mutex;
    int pin_csn, pin_ce, pin_irq;

    uint8_t channel;
    nrf24_rate_t rate;
    nrf24_pa_t pa;
    uint8_t addr_width;
    bool powered;
    bool rx_mode;
    bool carrier_on;
    bool promiscuous;

    uint8_t saved_config;
    uint8_t saved_en_aa;
    uint8_t saved_en_rxaddr;
    uint8_t saved_setup_aw;
    uint8_t saved_rx_pw_p0;
    bool    saved_powered;
} nrf24_ctx_t;

static nrf24_ctx_t s_ctx;

static SemaphoreHandle_t s_radio_owner = NULL;
static volatile bool     s_radio_owned = false;
static char              s_radio_owner_name[16] = {0};

static inline void csn_low(void)  { gpio_set_level(s_ctx.pin_csn, 0); }
static inline void csn_high(void) { gpio_set_level(s_ctx.pin_csn, 1); }
static inline void ce_low(void)   { gpio_set_level(s_ctx.pin_ce, 0); }
static inline void ce_high(void)  { gpio_set_level(s_ctx.pin_ce, 1); }

static esp_err_t spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (!s_ctx.spi) return ESP_ERR_INVALID_STATE;
    if (!spi_shared_bus_radio_access_allowed()) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;
    if (len > 64) return ESP_ERR_INVALID_SIZE;

    uint8_t tx_buf[64];
    uint8_t rx_buf[64];

    if (tx) memcpy(tx_buf, tx, len);
    else    memset(tx_buf, 0xFF, len);
    memset(rx_buf, 0x00, len);

    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };

    esp_err_t err = spi_device_acquire_bus(s_ctx.spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    if (!spi_shared_bus_radio_access_allowed()) {
        spi_device_release_bus(s_ctx.spi);
        return ESP_ERR_INVALID_STATE;
    }
    csn_low();
    err = spi_device_polling_transmit(s_ctx.spi, &t);
    csn_high();
    spi_device_release_bus(s_ctx.spi);

    if (err != ESP_OK) return err;
    if (rx) memcpy(rx, rx_buf, len);
    return ESP_OK;
}

static uint8_t read_reg_unlocked(uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(NRF24_CMD_R_REGISTER | (reg & 0x1F)), 0xFF };
    uint8_t rx[2] = {0};
    if (spi_xfer(tx, rx, 2) != ESP_OK) return 0;
    return rx[1];
}

static esp_err_t write_reg_unlocked(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(NRF24_CMD_W_REGISTER | (reg & 0x1F)), val };
    return spi_xfer(tx, NULL, 2);
}

static esp_err_t read_buf_unlocked(uint8_t reg, uint8_t *buf, size_t len)
{
    if (len == 0 || len > NRF24_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    uint8_t tx[1 + NRF24_MAX_PAYLOAD];
    uint8_t rx[1 + NRF24_MAX_PAYLOAD];
    tx[0] = (uint8_t)(NRF24_CMD_R_REGISTER | (reg & 0x1F));
    memset(tx + 1, 0xFF, len);
    esp_err_t err = spi_xfer(tx, rx, 1 + len);
    if (err == ESP_OK) memcpy(buf, rx + 1, len);
    return err;
}

static esp_err_t write_buf_unlocked(uint8_t reg, const uint8_t *buf, size_t len)
{
    if (len == 0 || len > NRF24_MAX_PAYLOAD) return ESP_ERR_INVALID_SIZE;
    uint8_t tx[1 + NRF24_MAX_PAYLOAD];
    tx[0] = (uint8_t)(NRF24_CMD_W_REGISTER | (reg & 0x1F));
    memcpy(tx + 1, buf, len);
    return spi_xfer(tx, NULL, 1 + len);
}

static esp_err_t spi_cmd_unlocked(uint8_t cmd)
{
    return spi_xfer(&cmd, NULL, 1);
}

static inline bool lock(void)
{
    return s_ctx.mutex && xSemaphoreTakeRecursive(s_ctx.mutex, portMAX_DELAY) == pdTRUE;
}

static inline void unlock(void)
{
    if (s_ctx.mutex) xSemaphoreGiveRecursive(s_ctx.mutex);
}

static void clear_carrier_unlocked(void)
{
    if (!s_ctx.carrier_on) return;
    uint8_t rf = read_reg_unlocked(NRF24_REG_RF_SETUP);
    rf &= ~(NRF24_RF_CONT_WAVE | NRF24_RF_PLL_LOCK);
    write_reg_unlocked(NRF24_REG_RF_SETUP, rf);
    s_ctx.carrier_on = false;
}

bool nrf24_radio_busy(void)
{
    return s_radio_owned;
}

bool nrf24_radio_claim(const char *owner)
{
    if (!s_radio_owner || !s_ctx.initialized || !s_ctx.present ||
        !spi_shared_bus_radio_access_allowed()) return false;

    /* FIX H1: fail-fast — timeout 0, tidak blocking */
    if (xSemaphoreTake(s_radio_owner, 0) != pdTRUE) {
        return false;
    }

    s_radio_owned = true;
    strncpy(s_radio_owner_name, owner ? owner : "?",
            sizeof(s_radio_owner_name) - 1);
    s_radio_owner_name[sizeof(s_radio_owner_name) - 1] = '\0';
    ESP_LOGI(TAG, "radio claimed by %s", s_radio_owner_name);
    if (nrf24_power_up() != ESP_OK) {
        s_radio_owned = false;
        s_radio_owner_name[0] = '\0';
        xSemaphoreGive(s_radio_owner);
        return false;
    }
    return true;
}

void nrf24_radio_release(void)
{
    if (!s_radio_owner) return;
    if (!s_radio_owned) return;
    esp_err_t power_err = nrf24_power_down();
    if (power_err != ESP_OK) {
        ESP_LOGW(TAG, "power-down on release failed: %s",
                 esp_err_to_name(power_err));
    }
    ESP_LOGI(TAG, "radio released (was %s)", s_radio_owner_name);
    s_radio_owned = false;
    s_radio_owner_name[0] = '\0';
    xSemaphoreGive(s_radio_owner);
}

esp_err_t nrf24_init(const nrf24_pins_t *pins)
{
    if (!pins) return ESP_ERR_INVALID_ARG;
    if (s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (pins->pin_csn < 0 || pins->pin_ce < 0) return ESP_ERR_INVALID_ARG;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.pin_csn = pins->pin_csn;
    s_ctx.pin_ce  = pins->pin_ce;
    s_ctx.pin_irq = pins->pin_irq;
    s_ctx.channel = 76;
    s_ctx.rate = NRF24_RATE_1M;
    s_ctx.pa = NRF24_PA_MAX;
    s_ctx.addr_width = 5;

    s_ctx.mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_ctx.mutex) return ESP_ERR_NO_MEM;

    s_radio_owner = xSemaphoreCreateBinary();
    if (!s_radio_owner) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_radio_owner);
    s_radio_owned = false;

    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << pins->pin_csn),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(pins->pin_csn, 1);
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) goto fail;

    io.pin_bit_mask = (1ULL << pins->pin_ce);
    gpio_set_level(pins->pin_ce, 0);
    err = gpio_config(&io);
    if (err != ESP_OK) goto fail;

    if (pins->pin_irq >= 0) {
        io.mode = GPIO_MODE_INPUT;
        io.pin_bit_mask = (1ULL << pins->pin_irq);
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io);
    }

    uint32_t clk = pins->spi_clock_hz ? pins->spi_clock_hz : 8000000;
    if (clk > NRF24_SPI_CLOCK_MAX_HZ) clk = NRF24_SPI_CLOCK_MAX_HZ;

    spi_bus_config_t bus = {
        .mosi_io_num = pins->pin_mosi,
        .miso_io_num = pins->pin_miso,
        .sclk_io_num = pins->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };

    if (!spi_shared_bus_is_ready()) {
        err = spi_bus_initialize(NRF24_SPI_HOST, &bus, SPI_DMA_DISABLED);
        if (err == ESP_OK) s_ctx.bus_owned = true;
        else if (err == ESP_ERR_INVALID_STATE) s_ctx.bus_owned = false;
        else goto fail;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = clk,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = spi_bus_add_device(NRF24_SPI_HOST, &dev, &s_ctx.spi);
    if (err != ESP_OK) goto fail;

    s_ctx.initialized = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    err = nrf24_selftest();
    s_ctx.present = (err == ESP_OK);

    if (!s_ctx.present) {
        ESP_LOGE(TAG, "nRF24 NOT detected");
        return ESP_OK;
    }

    lock();
    bool ok = true;
    ok &= (write_reg_unlocked(NRF24_REG_CONFIG, NRF24_CONFIG_EN_CRC) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_EN_AA, 0x00) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_EN_RXADDR, 0x03) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_SETUP_AW, 0x03) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_SETUP_RETR, 0x00) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_RF_CH, s_ctx.channel) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_RF_SETUP, NRF24_RF_PWR_0DBM) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_RX_PW_P0, 32) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_RX_PW_P1, 32) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_FEATURE, NRF24_FEATURE_EN_DYN_ACK) == ESP_OK);
    ok &= (write_reg_unlocked(NRF24_REG_STATUS, 0x70) == ESP_OK);
    spi_cmd_unlocked(NRF24_CMD_FLUSH_TX);
    spi_cmd_unlocked(NRF24_CMD_FLUSH_RX);
    uint8_t ch_back = read_reg_unlocked(NRF24_REG_RF_CH);
    if (ch_back != s_ctx.channel) ok = false;
    unlock();

    if (!ok) {
        s_ctx.present = false;
        ESP_LOGE(TAG, "config write failed");
    } else {
        nrf24_power_up();
        ESP_LOGI(TAG, "init OK (present=1)");
    }
    return ESP_OK;

fail:
    if (s_ctx.spi) spi_bus_remove_device(s_ctx.spi);
    if (s_ctx.bus_owned) spi_bus_free(NRF24_SPI_HOST);
    if (s_ctx.mutex) vSemaphoreDelete(s_ctx.mutex);
    if (s_radio_owner) {
        vSemaphoreDelete(s_radio_owner);
        s_radio_owner = NULL;          /* avoid dangling ptr if init retried */
        s_radio_owned = false;
        s_radio_owner_name[0] = '\0';
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    return err;
}

esp_err_t nrf24_deinit(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    ce_low();
    nrf24_power_down();
    if (s_ctx.spi) spi_bus_remove_device(s_ctx.spi);
    if (s_ctx.bus_owned) spi_bus_free(NRF24_SPI_HOST);
    if (s_ctx.mutex) vSemaphoreDelete(s_ctx.mutex);
    if (s_radio_owner) {
        vSemaphoreDelete(s_radio_owner);
        s_radio_owner = NULL;          /* avoid use-after-free on re-init */
        s_radio_owned = false;
        s_radio_owner_name[0] = '\0';
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}

bool nrf24_is_initialized(void) { return s_ctx.initialized; }
bool nrf24_is_present(void)     { return s_ctx.initialized && s_ctx.present; }

esp_err_t nrf24_selftest(void)
{
    if (!s_ctx.initialized || !s_ctx.spi) return ESP_ERR_INVALID_STATE;

    lock();
    uint8_t orig_ch = read_reg_unlocked(NRF24_REG_RF_CH);
    uint8_t orig_aw = read_reg_unlocked(NRF24_REG_SETUP_AW);

    write_reg_unlocked(NRF24_REG_RF_CH, 0x00);
    uint8_t r1 = read_reg_unlocked(NRF24_REG_RF_CH);
    write_reg_unlocked(NRF24_REG_RF_CH, 0x7D);
    uint8_t r2 = read_reg_unlocked(NRF24_REG_RF_CH);
    write_reg_unlocked(NRF24_REG_SETUP_AW, 0x01);
    uint8_t r3 = read_reg_unlocked(NRF24_REG_SETUP_AW);
    write_reg_unlocked(NRF24_REG_SETUP_AW, 0x03);
    uint8_t r4 = read_reg_unlocked(NRF24_REG_SETUP_AW);

    write_reg_unlocked(NRF24_REG_RF_CH, orig_ch);
    write_reg_unlocked(NRF24_REG_SETUP_AW, orig_aw);
    unlock();

    bool ok = (r1 == 0x00) && (r2 == 0x7D) && (r3 == 0x01) && (r4 == 0x03);
    if (!ok) ESP_LOGE(TAG, "selftest fail: %02X %02X %02X %02X", r1, r2, r3, r4);
    return ok ? ESP_OK : ESP_FAIL;
}

uint8_t nrf24_read_reg(uint8_t reg)
{
    if (!s_ctx.initialized) return 0;
    lock();
    uint8_t v = read_reg_unlocked(reg);
    unlock();
    return v;
}

esp_err_t nrf24_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t e = write_reg_unlocked(reg, val);
    unlock();
    return e;
}

esp_err_t nrf24_read_buf(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_ctx.initialized || !buf) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t e = read_buf_unlocked(reg, buf, len);
    unlock();
    return e;
}

esp_err_t nrf24_write_buf(uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!s_ctx.initialized || !buf) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t e = write_buf_unlocked(reg, buf, len);
    unlock();
    return e;
}

uint8_t nrf24_status(void)
{
    if (!s_ctx.initialized) return 0;
    uint8_t cmd = NRF24_CMD_NOP, rx = 0;
    lock();
    spi_xfer(&cmd, &rx, 1);
    unlock();
    return rx;
}

uint8_t nrf24_fifo_status(void) { return nrf24_read_reg(NRF24_REG_FIFO_STATUS); }

void nrf24_flush_tx(void)
{
    if (!s_ctx.initialized) return;
    lock(); spi_cmd_unlocked(NRF24_CMD_FLUSH_TX); unlock();
}

void nrf24_flush_rx(void)
{
    if (!s_ctx.initialized) return;
    lock(); spi_cmd_unlocked(NRF24_CMD_FLUSH_RX); unlock();
}

void nrf24_clear_irq(void)
{
    if (!s_ctx.initialized) return;
    lock(); write_reg_unlocked(NRF24_REG_STATUS, 0x70); unlock();
}

esp_err_t nrf24_set_channel(uint8_t ch)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (ch > 125) return ESP_ERR_INVALID_ARG;

    lock();
    bool active = s_ctx.carrier_on || s_ctx.rx_mode;
    if (active) { ce_low(); esp_rom_delay_us(5); }

    esp_err_t err = write_reg_unlocked(NRF24_REG_RF_CH, ch);
    if (err == ESP_OK) {
        s_ctx.channel = ch;
        esp_rom_delay_us(NRF24_CH_SETTLE_US);
    }
    if (active) ce_high();
    unlock();
    return err;
}

uint8_t nrf24_get_channel(void) { return s_ctx.channel; }

esp_err_t nrf24_set_data_rate(nrf24_rate_t r)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (r > NRF24_RATE_2M) return ESP_ERR_INVALID_ARG;
    lock();
    uint8_t rf = read_reg_unlocked(NRF24_REG_RF_SETUP);
    rf &= ~(NRF24_RF_DR_HIGH | NRF24_RF_DR_LOW);
    if (r == NRF24_RATE_250K) rf |= NRF24_RF_DR_LOW;
    else if (r == NRF24_RATE_2M) rf |= NRF24_RF_DR_HIGH;
    esp_err_t err = write_reg_unlocked(NRF24_REG_RF_SETUP, rf);
    if (err == ESP_OK) s_ctx.rate = r;
    unlock();
    return err;
}

nrf24_rate_t nrf24_get_data_rate(void) { return s_ctx.rate; }

esp_err_t nrf24_set_pa_level(nrf24_pa_t pa)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (pa > NRF24_PA_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t pwr;
    switch (pa) {
        case NRF24_PA_MIN:  pwr = NRF24_RF_PWR_M18DBM; break;
        case NRF24_PA_LOW:  pwr = NRF24_RF_PWR_M12DBM; break;
        case NRF24_PA_HIGH: pwr = NRF24_RF_PWR_M6DBM;  break;
        default:            pwr = NRF24_RF_PWR_0DBM;   break;
    }
    lock();
    uint8_t rf = read_reg_unlocked(NRF24_REG_RF_SETUP);
    rf = (rf & ~0x06) | pwr;
    esp_err_t err = write_reg_unlocked(NRF24_REG_RF_SETUP, rf);
    if (err == ESP_OK) s_ctx.pa = pa;
    unlock();
    return err;
}

nrf24_pa_t nrf24_get_pa_level(void) { return s_ctx.pa; }

esp_err_t nrf24_set_addr_width(nrf24_aw_t aw)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (aw < NRF24_AW_3 || aw > NRF24_AW_5) return ESP_ERR_INVALID_ARG;
    uint8_t v = (aw == NRF24_AW_3) ? 0x01 : (aw == NRF24_AW_4) ? 0x02 : 0x03;
    lock();
    esp_err_t err = write_reg_unlocked(NRF24_REG_SETUP_AW, v);
    if (err == ESP_OK) s_ctx.addr_width = aw;
    unlock();
    return err;
}

nrf24_aw_t nrf24_get_addr_width(void) { return s_ctx.addr_width; }

esp_err_t nrf24_set_tx_address(const uint8_t *addr, size_t len)
{
    if (!s_ctx.initialized || !addr) return ESP_ERR_INVALID_ARG;
    if (len < 3 || len > 5 || len != s_ctx.addr_width) return ESP_ERR_INVALID_ARG;
    return nrf24_write_buf(NRF24_REG_TX_ADDR, addr, len);
}

esp_err_t nrf24_set_rx_address(uint8_t pipe, const uint8_t *addr, size_t len)
{
    if (!s_ctx.initialized || !addr) return ESP_ERR_INVALID_ARG;
    if (pipe > 5 || len < 3 || len > 5 || len != s_ctx.addr_width)
        return ESP_ERR_INVALID_ARG;
    if (pipe >= 2) return nrf24_write_reg(NRF24_REG_RX_ADDR_P0 + pipe, addr[0]);
    return nrf24_write_buf(NRF24_REG_RX_ADDR_P0 + pipe, addr, len);
}

esp_err_t nrf24_set_rx_payload_size(uint8_t pipe, uint8_t size)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (pipe > 5 || size > 32) return ESP_ERR_INVALID_ARG;
    return nrf24_write_reg(NRF24_REG_RX_PW_P0 + pipe, size);
}

esp_err_t nrf24_enable_pipe(uint8_t pipe, bool en)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (pipe > 5) return ESP_ERR_INVALID_ARG;
    lock();
    uint8_t v = read_reg_unlocked(NRF24_REG_EN_RXADDR);
    if (en) v |= (1 << pipe); else v &= ~(1 << pipe);
    esp_err_t e = write_reg_unlocked(NRF24_REG_EN_RXADDR, v);
    unlock();
    return e;
}

esp_err_t nrf24_enable_auto_ack(uint8_t pipe, bool en)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (pipe > 5) return ESP_ERR_INVALID_ARG;
    lock();
    uint8_t v = read_reg_unlocked(NRF24_REG_EN_AA);
    if (en) v |= (1 << pipe); else v &= ~(1 << pipe);
    esp_err_t e = write_reg_unlocked(NRF24_REG_EN_AA, v);
    unlock();
    return e;
}

esp_err_t nrf24_enable_crc(bool en, bool two_bytes)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    lock();
    uint8_t v = read_reg_unlocked(NRF24_REG_CONFIG);
    if (en) {
        v |= NRF24_CONFIG_EN_CRC;
        if (two_bytes) v |= NRF24_CONFIG_CRCO;
        else           v &= ~NRF24_CONFIG_CRCO;
    } else {
        v &= ~NRF24_CONFIG_EN_CRC;
    }
    esp_err_t e = write_reg_unlocked(NRF24_REG_CONFIG, v);
    unlock();
    return e;
}

esp_err_t nrf24_set_retries(uint8_t delay_250us, uint8_t count)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (delay_250us > 15 || count > 15) return ESP_ERR_INVALID_ARG;
    return nrf24_write_reg(NRF24_REG_SETUP_RETR, (uint8_t)((delay_250us << 4) | count));
}

static esp_err_t set_feature_bit(uint8_t bit, bool en)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    lock();
    uint8_t v = read_reg_unlocked(NRF24_REG_FEATURE);
    if (en) v |= bit; else v &= ~bit;
    esp_err_t e = write_reg_unlocked(NRF24_REG_FEATURE, v);
    unlock();
    return e;
}

esp_err_t nrf24_enable_dynamic_ack(bool en)     { return set_feature_bit(NRF24_FEATURE_EN_DYN_ACK, en); }
esp_err_t nrf24_enable_dynamic_payload(bool en) { return set_feature_bit(NRF24_FEATURE_EN_DPL, en); }
esp_err_t nrf24_enable_ack_payload(bool en)     { return set_feature_bit(NRF24_FEATURE_EN_ACK_PAY, en); }

esp_err_t nrf24_power_up(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    lock();
    uint8_t v = read_reg_unlocked(NRF24_REG_CONFIG);
    v |= NRF24_CONFIG_PWR_UP;
    esp_err_t err = write_reg_unlocked(NRF24_REG_CONFIG, v);
    if (err == ESP_OK) s_ctx.powered = true;
    unlock();
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(NRF24_POWERUP_DELAY_MS));
    return err;
}

esp_err_t nrf24_power_down(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    ce_low();
    esp_rom_delay_us(100);
    lock();
    clear_carrier_unlocked();
    uint8_t v = read_reg_unlocked(NRF24_REG_CONFIG);
    v &= ~NRF24_CONFIG_PWR_UP;
    esp_err_t err = write_reg_unlocked(NRF24_REG_CONFIG, v);
    s_ctx.powered = false;
    s_ctx.rx_mode = false;
    unlock();
    return err;
}

bool nrf24_is_powered(void) { return s_ctx.powered; }

esp_err_t nrf24_enter_tx(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (!s_ctx.powered) { esp_err_t e = nrf24_power_up(); if (e) return e; }
    ce_low();
    esp_rom_delay_us(100);
    lock();
    clear_carrier_unlocked();
    uint8_t v = read_reg_unlocked(NRF24_REG_CONFIG);
    v &= ~NRF24_CONFIG_PRIM_RX;
    esp_err_t err = write_reg_unlocked(NRF24_REG_CONFIG, v);
    s_ctx.rx_mode = false;
    unlock();
    return err;
}

esp_err_t nrf24_enter_rx(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (!s_ctx.powered) { esp_err_t e = nrf24_power_up(); if (e) return e; }
    ce_low();
    esp_rom_delay_us(100);
    lock();
    clear_carrier_unlocked();
    spi_cmd_unlocked(NRF24_CMD_FLUSH_RX);
    write_reg_unlocked(NRF24_REG_STATUS, 0x70);
    uint8_t v = read_reg_unlocked(NRF24_REG_CONFIG);
    v |= NRF24_CONFIG_PRIM_RX;
    esp_err_t err = write_reg_unlocked(NRF24_REG_CONFIG, v);
    if (err == ESP_OK) { s_ctx.rx_mode = true; ce_high(); }
    unlock();
    if (err == ESP_OK) esp_rom_delay_us(NRF24_CH_SETTLE_US);
    return err;
}

esp_err_t nrf24_standby(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    ce_low();
    esp_rom_delay_us(100);
    lock();
    s_ctx.rx_mode = false;
    unlock();
    return ESP_OK;
}

bool nrf24_is_rx_mode(void) { return s_ctx.rx_mode; }

static esp_err_t tx_common(const uint8_t *data, size_t len, bool no_ack, uint32_t timeout_us)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0 || len > 32) return ESP_ERR_INVALID_ARG;

    if (s_ctx.rx_mode) { esp_err_t e = nrf24_enter_tx(); if (e) return e; }

    lock();
    if (s_ctx.carrier_on) { unlock(); return ESP_ERR_INVALID_STATE; }

    spi_cmd_unlocked(NRF24_CMD_FLUSH_TX);
    write_reg_unlocked(NRF24_REG_STATUS, 0x30);

    uint8_t tx[1 + 32];
    tx[0] = no_ack ? NRF24_CMD_W_TX_PAYLOAD_NOACK : NRF24_CMD_W_TX_PAYLOAD;
    memcpy(tx + 1, data, len);
    esp_err_t err = spi_xfer(tx, NULL, 1 + len);
    if (err != ESP_OK) { unlock(); return err; }

    ce_high();
    esp_rom_delay_us(NRF24_CE_PULSE_US);
    ce_low();
    unlock();

    if (timeout_us == 0) return ESP_OK;

    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < (int64_t)timeout_us) {
        uint8_t st = nrf24_status();
        if (st & (NRF24_STATUS_TX_DS | NRF24_STATUS_MAX_RT)) break;
        esp_rom_delay_us(50);
    }
    return ESP_OK;
}

esp_err_t nrf24_tx(const uint8_t *data, size_t len)
{
    return tx_common(data, len, false, NRF24_TX_DEFAULT_TIMEOUT_US);
}

esp_err_t nrf24_tx_no_ack(const uint8_t *data, size_t len)
{
    return tx_common(data, len, true, 200);
}

esp_err_t nrf24_tx_wait(const uint8_t *data, size_t len, uint32_t timeout_us)
{
    return tx_common(data, len, false, timeout_us);
}

bool nrf24_rx_available(void)
{
    if (!s_ctx.initialized) return false;
    return (nrf24_fifo_status() & NRF24_FIFO_RX_EMPTY) == 0;
}

int nrf24_rx(uint8_t *data, size_t max_len)
{
    if (!s_ctx.initialized || !data || max_len == 0) return -1;
    lock();
    if (!nrf24_rx_available()) { unlock(); return -1; }

    uint8_t tx_buf[1 + 32];
    uint8_t rx_buf[1 + 32];
    tx_buf[0] = NRF24_CMD_R_RX_PAYLOAD;
    memset(tx_buf + 1, 0xFF, 32);
    memset(rx_buf, 0, sizeof(rx_buf));

    esp_err_t err = spi_xfer(tx_buf, rx_buf, 1 + 32);
    unlock();
    if (err != ESP_OK) return -1;

    size_t copy = (max_len < 32) ? max_len : 32;
    memcpy(data, rx_buf + 1, copy);
    return (int)copy;
}

bool nrf24_get_rpd(void)
{
    if (!s_ctx.initialized) return false;
    if (!s_ctx.rx_mode && !s_ctx.promiscuous) return false;
    return (nrf24_read_reg(NRF24_REG_RPD) & 0x01) != 0;
}

esp_err_t nrf24_sample_rpd_window(bool *detected)
{
    if (!s_ctx.initialized || !s_ctx.powered || !s_ctx.rx_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!detected) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_INVALID_STATE;

    ce_low();
    esp_rom_delay_us(5);
    uint8_t tx[2] = { NRF24_CMD_R_REGISTER | NRF24_REG_RPD, 0xFF };
    uint8_t rx[2] = {0};
    esp_err_t err = spi_xfer(tx, rx, sizeof(tx));
    ce_high();
    esp_rom_delay_us(170);
    unlock();
    if (err != ESP_OK) return err;
    *detected = (rx[1] & 0x01) != 0;
    return ESP_OK;
}

esp_err_t nrf24_start_carrier(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (!s_ctx.powered) { esp_err_t e = nrf24_power_up(); if (e) return e; }
    ce_low();
    esp_rom_delay_us(100);

    lock();
    uint8_t cfg = read_reg_unlocked(NRF24_REG_CONFIG);
    cfg &= ~NRF24_CONFIG_PRIM_RX;
    cfg |= NRF24_CONFIG_PWR_UP;
    write_reg_unlocked(NRF24_REG_CONFIG, cfg);
    uint8_t rf = read_reg_unlocked(NRF24_REG_RF_SETUP);
    rf |= NRF24_RF_CONT_WAVE | NRF24_RF_PLL_LOCK;
    esp_err_t err = write_reg_unlocked(NRF24_REG_RF_SETUP, rf);
    if (err == ESP_OK) {
        s_ctx.carrier_on = true;
        s_ctx.rx_mode = false;
        ce_high();
    }
    unlock();
    if (err == ESP_OK) esp_rom_delay_us(NRF24_CH_SETTLE_US);
    return err;
}

esp_err_t nrf24_stop_carrier(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    ce_low();
    esp_rom_delay_us(50);
    lock();
    clear_carrier_unlocked();
    unlock();
    return ESP_OK;
}

bool nrf24_is_carrier_on(void) { return s_ctx.carrier_on; }

esp_err_t nrf24_enter_promiscuous(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (s_ctx.promiscuous) return ESP_OK;
    ce_low();
    esp_rom_delay_us(100);

    lock();
    s_ctx.saved_config    = read_reg_unlocked(NRF24_REG_CONFIG);
    s_ctx.saved_en_aa     = read_reg_unlocked(NRF24_REG_EN_AA);
    s_ctx.saved_en_rxaddr = read_reg_unlocked(NRF24_REG_EN_RXADDR);
    s_ctx.saved_setup_aw  = read_reg_unlocked(NRF24_REG_SETUP_AW);
    s_ctx.saved_rx_pw_p0  = read_reg_unlocked(NRF24_REG_RX_PW_P0);
    s_ctx.saved_powered   = s_ctx.powered;

    uint8_t cfg = read_reg_unlocked(NRF24_REG_CONFIG);
    cfg &= ~NRF24_CONFIG_EN_CRC;
    cfg |= NRF24_CONFIG_PWR_UP | NRF24_CONFIG_PRIM_RX;
    write_reg_unlocked(NRF24_REG_CONFIG, cfg);
    write_reg_unlocked(NRF24_REG_EN_AA, 0x00);
    write_reg_unlocked(NRF24_REG_EN_RXADDR, 0x01);
    write_reg_unlocked(NRF24_REG_SETUP_AW, 0x00);
    write_reg_unlocked(NRF24_REG_RX_PW_P0, 32);
    spi_cmd_unlocked(NRF24_CMD_FLUSH_RX);
    write_reg_unlocked(NRF24_REG_STATUS, 0x70);

    s_ctx.promiscuous = true;
    s_ctx.rx_mode = true;
    s_ctx.powered = true;
    ce_high();
    unlock();
    esp_rom_delay_us(NRF24_CH_SETTLE_US);
    return ESP_OK;
}

esp_err_t nrf24_exit_promiscuous(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (!s_ctx.promiscuous) return ESP_OK;
    ce_low();
    esp_rom_delay_us(100);

    lock();
    write_reg_unlocked(NRF24_REG_CONFIG,    s_ctx.saved_config);
    write_reg_unlocked(NRF24_REG_EN_AA,     s_ctx.saved_en_aa);
    write_reg_unlocked(NRF24_REG_EN_RXADDR, s_ctx.saved_en_rxaddr);
    write_reg_unlocked(NRF24_REG_SETUP_AW,  s_ctx.saved_setup_aw);
    write_reg_unlocked(NRF24_REG_RX_PW_P0,  s_ctx.saved_rx_pw_p0);
    spi_cmd_unlocked(NRF24_CMD_FLUSH_RX);
    spi_cmd_unlocked(NRF24_CMD_FLUSH_TX);

    s_ctx.promiscuous = false;
    s_ctx.rx_mode = false;
    s_ctx.powered = s_ctx.saved_powered;
    s_ctx.addr_width = 5;
    unlock();
    return ESP_OK;
}

bool nrf24_is_promiscuous(void) { return s_ctx.promiscuous; }

void nrf24_dump_status(void)
{
    if (!s_ctx.initialized) { ESP_LOGE(TAG, "not initialized"); return; }
    lock();
    uint8_t cmd = NRF24_CMD_NOP, st = 0;
    spi_xfer(&cmd, &st, 1);
    uint8_t cfg = read_reg_unlocked(NRF24_REG_CONFIG);
    uint8_t aa  = read_reg_unlocked(NRF24_REG_EN_AA);
    uint8_t ch  = read_reg_unlocked(NRF24_REG_RF_CH);
    uint8_t rf  = read_reg_unlocked(NRF24_REG_RF_SETUP);
    uint8_t fifo = read_reg_unlocked(NRF24_REG_FIFO_STATUS);
    uint8_t feat = read_reg_unlocked(NRF24_REG_FEATURE);
    unlock();
    ESP_LOGI(TAG, "ST=0x%02X CFG=0x%02X AA=0x%02X CH=%u RF=0x%02X FIFO=0x%02X FEAT=0x%02X",
             st, cfg, aa, ch, rf, fifo, feat);
}
