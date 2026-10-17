#include "transport_serial.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "hal/usb_serial_jtag_ll.h"

#include "io.h"
#include "cli.h"

/* Direct hardware-FIFO transport with non-blocking sink.
 *
 * The original design called fifo_write() directly from serial_sink(), which
 * blocks (via vTaskDelay) when the USB TX FIFO is full (i.e. no host reading).
 * serial_sink() is called while the io mutex is held, so any other task that
 * tried to log during that window — including the NimBLE host task — would
 * deadlock waiting for the mutex. This prevented BLE from ever syncing.
 *
 * Fix: mirror the BLE transport pattern. serial_sink() puts data into a
 * stream buffer (non-blocking, drops if full). A dedicated tx_task drains
 * the stream buffer to the USB FIFO — blocking is fine there since it runs
 * in its own task context without holding any shared lock.
 */

#define TX_BUF_BYTES 4096

static StreamBufferHandle_t s_tx_sb;
static portMUX_TYPE s_drop_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_tx_dropped_bytes;

static void fifo_write(const uint8_t *buf, size_t len)
{
    size_t i = 0;
    while (i < len) {
        if (usb_serial_jtag_ll_txfifo_writable()) {
            int n = usb_serial_jtag_ll_write_txfifo(buf + i, len - i);
            if (n > 0) i += n;
            usb_serial_jtag_ll_txfifo_flush();
        } else {
            vTaskDelay(1);
        }
    }
}

static void serial_sink(const char *buf, size_t len, void *ctx)
{
    (void)ctx;
    if (!buf || !len) return;
    size_t queued = xStreamBufferSend(s_tx_sb, buf, len, 0);
    if (queued != len) {
        portENTER_CRITICAL(&s_drop_lock);
        s_tx_dropped_bytes += (uint32_t)(len - queued);
        portEXIT_CRITICAL(&s_drop_lock);
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_tx_sb, buf, sizeof(buf), portMAX_DELAY);
        if (n > 0) {
            portENTER_CRITICAL(&s_drop_lock);
            uint32_t dropped = s_tx_dropped_bytes;
            s_tx_dropped_bytes = 0;
            portEXIT_CRITICAL(&s_drop_lock);
            if (dropped > 0) {
                io_log("serial: previously dropped %u TX bytes\r\n",
                       (unsigned)dropped);
            }
            fifo_write(buf, n);
        }
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    char line[256];
    size_t line_len = 0;
    bool discarding_line = false;
    for (;;) {
        int n = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char c = (char)buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    if (discarding_line) {
                        discarding_line = false;
                        line_len = 0;
                    } else {
                        line[line_len++] = '\n';
                        cli_feed(line, line_len);
                        line_len = 0;
                    }
                    continue;
                }
                if (discarding_line) continue;
                if (line_len < sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    line_len = 0;
                    discarding_line = true;
                    io_log("serial: input line too long; input dropped\r\n");
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

bool transport_serial_init(void)
{
    s_tx_sb = xStreamBufferCreate(TX_BUF_BYTES, 1);
    if (!s_tx_sb) return false;

    TaskHandle_t tx_task_handle = NULL;
    TaskHandle_t rx_task_handle = NULL;
    if (xTaskCreate(tx_task, "ser_tx", 2048, NULL, 8,
                    &tx_task_handle) != pdPASS) {
        vStreamBufferDelete(s_tx_sb);
        s_tx_sb = NULL;
        return false;
    }
    if (xTaskCreate(rx_task, "ser_rx", 4096, NULL, 8,
                    &rx_task_handle) != pdPASS) {
        vTaskDelete(tx_task_handle);
        vStreamBufferDelete(s_tx_sb);
        s_tx_sb = NULL;
        return false;
    }
    if (io_register_sink(serial_sink, NULL) != 0) {
        vTaskDelete(rx_task_handle);
        vTaskDelete(tx_task_handle);
        vStreamBufferDelete(s_tx_sb);
        s_tx_sb = NULL;
        return false;
    }
    return true;
}
