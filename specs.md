# ESP32-C5-DevKitC-1 N16R8 - Specs

Hardware reference for the Espressif ESP32-C5-DevKitC-1 fitted with an
ESP32-C5-WROOM-1-N16R8 module. The project configuration targets ESP32-C5
with 16 MB flash and 8 MB quad SPI PSRAM.

- [ESP32-C5-DevKitC-1 official user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp32-c5-devkitc-1/user_guide.html)
- [ESP32-C5-WROOM-1 / WROOM-1U datasheet](https://documentation.espressif.com/esp32-c5-wroom-1_wroom-1u_datasheet_en.html)
- [ESP32-C5 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf)

## Chip and module

| Item | Value |
|---|---|
| Module | ESP32-C5-WROOM-1-N16R8 |
| MCU | ESP32-C5, single-core 32-bit RISC-V |
| Maximum clock | 240 MHz |
| Flash | 16 MB, Quad SPI |
| PSRAM | 8 MB, Quad SPI |
| Wi-Fi | Wi-Fi 6 (802.11ax), 2.4 GHz and 5 GHz |
| Bluetooth | Bluetooth 5 LE |

The `N16R8` module variant identifies 16 MB flash and 8 MB PSRAM. Confirm the
module marking on the physical board if its exact variant is uncertain.

## Board

| Item | Value |
|---|---|
| Board | ESP32-C5-DevKitC-1 |
| Power input | 5 V via either USB Type-C port; 3.3 V via the 3V3 header |
| USB | ESP32-C5 USB Type-C and USB-to-UART Type-C ports |
| Onboard indicator | Addressable RGB LED on GPIO27 |

The pin headers are named **J1** and **J3** in Espressif's v1.2 user guide.
Pin numbers below are the header positions, not GPIO numbers.

## J1 header

| Pin | Label | Function / GPIO | Notes |
|---:|---|---|---|
| 1 | 3V3 | 3.3 V power | |
| 2 | RST | Reset | Active low |
| 3 | 2 | GPIO2 / MTMS | Strapping pin |
| 4 | 3 | GPIO3 / MTDI | Strapping pin |
| 5 | 0 | GPIO0 | |
| 6 | 1 | GPIO1 / ADC1_CH0 | |
| 7 | 6 | GPIO6 / ADC1_CH5 | |
| 8 | 7 | GPIO7 | Strapping pin |
| 9 | 8 | GPIO8 | |
| 10 | 9 | GPIO9 | |
| 11 | 10 | GPIO10 | |
| 12 | 26 | GPIO26 | Strapping pin |
| 13 | 25 | GPIO25 | Strapping pin |
| 14 | 5V | 5 V power | |
| 15 | G | Ground | |
| 16 | NC | Not connected | |

## J3 header

| Pin | Label | Function / GPIO | Notes |
|---:|---|---|---|
| 1 | G | Ground | |
| 2 | TX | U0TXD / GPIO11 | UART0 TX |
| 3 | RX | U0RXD / GPIO12 | UART0 RX |
| 4 | 24 | GPIO24 | |
| 5 | 23 | GPIO23 | |
| 6 | NC/15 | GPIO15 | Not available when using the module's SPI PSRAM |
| 7 | 27 | GPIO27 | Onboard RGB LED; strapping pin |
| 8 | 4 | GPIO4 / MTCK | |
| 9 | 5 | GPIO5 / MTDO | |
| 10 | NC | Not connected | |
| 11 | 28 | GPIO28 | Strapping pin |
| 12 | G | Ground | |
| 13 | 14 | GPIO14 / USB_D+ / SDIO_DATA2 | |
| 14 | 13 | GPIO13 / USB_D- / SDIO_DATA3 | |
| 15 | G | Ground | |
| 16 | NC | Not connected | |

## Pin notes

- GPIO2 (MTMS), GPIO3 (MTDI), GPIO7, GPIO25, GPIO26, GPIO27, and GPIO28 are
  strapping pins. Avoid external circuits that change their levels during
  power-up or reset.
- GPIO15 is unavailable for external use on modules with SPI PSRAM, including
  the N16R8 variant.
- GPIO13 and GPIO14 are also assigned to USB D- and USB D+ on the chip.
- See Espressif's [board pin-layout diagram](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/_images/esp32-c5-devkitc-1-pin-layout_v1.2.png)
  and [header reference](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32c5/esp32-c5-devkitc-1/user_guide.html#hardware-reference)
  when identifying physical header positions.
