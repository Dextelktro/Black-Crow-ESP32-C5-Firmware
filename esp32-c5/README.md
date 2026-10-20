# ESP32-C5 Dual-Band Deauther

Firmware configured for ESP32-C5 N16R8 (16 MB flash, 8 MB quad PSRAM).
Educational / authorized
security testing only — use exclusively on networks you own or have
written permission to test.

## Quick start (VS Code)

Install the official `Espressif IDF` extension (id
`espressif.esp-idf-extension`) in VS Code, then open this folder.

The extension's status bar gives you:

- ESP-IDF: Build project — compiles
- ESP-IDF: Flash device — writes flash via the chip's built-in JTAG
- ESP-IDF: Monitor device — opens the serial console at 115200 baud

`.vscode/settings.json` points the extension at the C5 target and the
`board/esp32c5-builtin.cfg` OpenOCD config, so flashing goes over JTAG
and never touches the CDC console — no BOOT/RESET button presses
needed even when running firmware has the CDC channel busy.

The first time, also run the `Tasks: Run Task → Patch libnet80211.a`
step (or `cp patched_libnet/libnet80211.a $IDF_PATH/components/esp_wifi/lib/esp32c5/`)
so `esp_wifi_80211_tx` accepts spoofed-SA management frames. The
backup is saved as `libnet80211.a.orig`.

## Quick start (command line)

ESP-IDF v5.5.1+ on PATH (`source ~/esp/esp-idf/export.sh`).

```bash
./flash.sh                  # build + JTAG-flash + soft-reset over serial
./flash.sh --no-build       # flash only
./flash.sh --full           # also reflash bootloader + partition table
./serial.sh                 # interactive CLI (DTR/RTS held LOW)
```

`flash.sh` flashes via OpenOCD against `board/esp32c5-builtin.cfg`,
which uses the chip's built-in JTAG side of the USB-Serial-JTAG
controller. The CDC side stays free, so the running firmware's CLI is
not interrupted during flashing. After a successful flash, `flash.sh`
sends `reset` to the running firmware, which calls `esp_restart()`.

## Project layout

```
esp32-c5/
├── CMakeLists.txt              IDF top-level
├── partitions.csv              factory (6 MB) + LittleFS storage (10 MB), 16 MB flash
├── sdkconfig.defaults          base IDF config (esp32c5, N16R8 flash/PSRAM, NimBLE)
├── patched_libnet/
│   └── libnet80211.a           drop-in replacement that enables spoofed-SA mgmt tx
├── flash.sh                    build + JTAG-flash + soft-reset
├── serial.sh                   interactive CLI (DTR=0 RTS=0)
├── tools/
│   ├── serial_console.py       Python TTY client used by serial.sh
│   └── generate_ir_universal_db.py  Convert Flipper category libraries to C
├── .vscode/
│   ├── settings.json           ESP-IDF extension config (target=esp32c5, JTAG flash)
│   ├── tasks.json              build / flash / monitor tasks
│   └── extensions.json         recommends espressif.esp-idf-extension
└── main/
    ├── CMakeLists.txt
    ├── main.c                  init order + boot status
    ├── temperature_monitor.{c,h}  internal die-temperature telemetry (4 Hz)
    ├── io.{c,h}                log fanout to all transports
    ├── cli.{c,h}               command parser (transport-agnostic)
    ├── transport_serial.{c,h}  USB-Serial-JTAG TX/RX
    ├── transport_ble.{c,h}     NimBLE NUS GATT peripheral and log transport
    ├── spi_shared_bus.{c,h}    shared SPI2 host for storage and radios
    ├── radio_modules.{c,h}     deferred radio initialization after SD mount
    ├── sd_storage.{c,h}        SD mount, files, copy, and download
    ├── cc1101.{c,h}            CC1101 SPI presence and sleep control
    ├── nrf24.{c,h}             nRF24L01+ SPI module
    ├── frames.{c,h}            802.11 deauth / disassoc / auth-flood builders
    ├── wifi_ctrl.{c,h}         STA-mode init + dual-band scan
    ├── sniffer.{c,h}           promiscuous STA discovery
    ├── targets.{c,h}           AP / STA / selection state
    ├── acl.{c,h}               whitelist + blacklist (NVS-persisted)
    ├── attack.{c,h}            attack scheduler
    ├── ir.{c,h}                IR feature: universal/receiver/emulate orchestration
    ├── ir_rmt.{c,h}            RMT TX/RX front-end (TX=GPIO4, RX=GPIO5, 38 kHz)
    ├── ir_proto.{c,h}          NEC/Samsung/Sony/RC5/RC6/RCA encode + decode
    ├── ir_db.{c,h}             built-in universal remote code library
    ├── ir_db_tv.inc            765 TV code entries from Flipper tv.ir
    ├── ir_db_tv_raw.inc        raw TV timing arrays (315 signals)
    ├── ir_db_ac.inc / ir_db_ac_raw.inc
    ├── ir_db_projector.inc / ir_db_projector_raw.inc
    ├── ir_db_led.inc / ir_db_led_raw.inc
    ├── ir_db_fan.inc / ir_db_fan_raw.inc
    ├── ir_db_audio.inc / ir_db_audio_raw.inc
    └── ir_store.{c,h}          LittleFS .ir save / list / load / emulate
```

## CLI commands

```
help                          show help
scan                          dual-band wifi scan
ls                            list APs
sniff <ap_idx> <sec>          promiscuous capture for that AP — discovers STAs
stas                          list captured STAs
t24 <ap_idx>                  set 2.4 GHz target
t5  <ap_idx>                  set 5 GHz target
sta <sta_idx>                 set unicast STA target
clear                         clear selected targets
mode <broadcast|unicast|disassoc|authflood|mixed>
start <duration_sec>
stop                          stop attack early
status
nrf status                    show nRF24 radio status
wl add <mac> [bssid|sta]      whitelist add (never deauth) — persisted
wl rm  <mac> | wl ls | wl clear
bl add <mac> [bssid|sta]      blacklist add (always deauth) — persisted
bl rm  <mac> | bl ls | bl clear
ir univ <tv|ac|projector|led|fan|audio> <button>   universal remote sweep
ir univ stop                  stop a universal sweep
ir rx start | ir rx stop      infrared receiver (capture + decode)
ir spectrum on | off          live IR spectrum envelope stream
ir save <name>                save the last captured signal to LittleFS
ir ls | ir info <name> | ir rm <name>              saved signal library
ir emulate <name> [count] | ir emulate stop        replay a saved signal
ir status                     IR module + storage status
sd status | ls                SD card status and file listing
sd mount | unmount | format   mount, safely eject, or format the card
sd get <path> | preview <path> download or preview an SD file
sd cp-to-lfs <path>           copy an SD file into LittleFS
sd cp-from-lfs <path>         copy a LittleFS file onto the SD card
espflash status               show UART flasher state
espflash flash <lfs|sd> <path> <offset> [chip|auto]
espflash batch begin <lfs|sd> <count> <chip|auto>
espflash batch image <index-0-based> <path> <offset> | start | abort
espflash mkdir <lfs|sd> <path> create an upload directory
espflash upload <lfs|sd> <path> <size> <crc32-hex>
espflash data <id> <offset> <hex> | finish <id> | cancel <id>
espflash cancel-active         remove the current incomplete upload
espflash boot | release       assert or release target BOOT (GPIO10)
espflash reset                reports unsupported while RESET is -1
cc1101 status | sleep | wake  CC1101 presence and low-power control
reset                         soft-reset the chip (esp_restart)
```

### UART ESP flasher

The optional serial flasher uses UART1 (host TX GPIO25, host RX GPIO3), target
BOOT GPIO10, and no automatic RESET line (`-1`). Cross target TX/RX and share
ground: host GPIO25 TX -> target UART RX, host GPIO3 RX <- target UART TX.
Use 3.3 V UART logic. These GPIO assignments must be checked against the exact
board revision's schematic before wiring. UART0/USB-Serial-JTAG is not claimed;
UART1 is installed only for a flash session and its driver is deleted on exit.
The flasher refuses to start if UART1 already has a driver.

Manual flash accepts up to three images in one operation, each with its own
image-specific offset, and optionally a chip profile. The target is detected
once; all source files, ranges, and offset overlaps are checked before the
first erase/write. Images are written sequentially and verified individually
before the next begins where target-side MD5 verification is supported
(ESP8266 ROM does not provide it). A failure stops the batch and is reported
explicitly; already written images cannot be rolled back. Data is streamed in
1 KiB blocks using PSRAM; UART/loader/task resources remain allocated only for
the active operation.

The Flutter flasher can select multiple files or an entire directory on Android
and desktop, then upload the selection to LittleFS or SD. Android folder
selection uses the Storage Access Framework and stages copies in app cache
without buffering files in RAM; the cache copies are removed after the
operation. Each file is streamed through the text NUS CLI in bounded
hexadecimal chunks (108 binary bytes per command), strict offsets, idempotent
retry of the last chunk, and CRC32 verification. A file is committed from its
`.part` file only after size and CRC verification; existing destination files
are never overwritten. Nested and empty directories are created explicitly.
Multi-file uploads commit one file at a time, so a later error leaves already
verified files in place and is reported as a partial transfer. LittleFS mounts
without formatting non-empty storage. SD uploads require an already mounted
card and reserve it exclusively only while each chunk is written; no SD lock or
open file is retained while waiting for the next BLE command. Per-file upload
size is limited by the ESP-IDF stdio seek range (2 GiB minus one byte).
If BLE disconnects during a transfer, `espflash status` reports the active
transfer and its offset; cancel it with `espflash cancel-active` to remove the
incomplete `.part` file. The Flutter flasher screen queries this status when
opened and exposes the remote cancellation action.

Quick Flash and Manual Flash use the same batch flow. No BIN profile is bundled
by this change; use the offsets from the selected firmware's own flash
configuration, and do not guess offsets for a multi-image firmware. A/B
switching remains unavailable until verified firmware images and target
partition/OTA manifests are packaged.

Universal sweeps report only completed RMT sends as `TX`; failed encodes or
transmissions are reported as `TX_FAIL`, and `UNIV_DONE` includes sent and
failed totals. Invalid or over-capacity timing data is rejected rather than
silently truncated. Stop requests finish the current IR signal before the
sweep exits; the app remains in a stopping state until `UNIV_DONE` arrives.

### nRF24L01+ wiring and link settings

Connect the module to 3.3 V logic (never 5 V) and a common ground:

| nRF24L01+ | ESP32-C5 GPIO |
|-----------|---------------|
| SCK       | 6             |
| MISO      | 2             |
| MOSI      | 7             |
| CSN       | 9             |
| CE        | 8             |

The radio is powered down while idle. Scanner, sniffer, and other nRF24
operations claim it exclusively and power it up until they release the radio.
`nrf status` reports whether initialization succeeded.

### Shared SPI, SD card, and CC1101

The SD card, nRF24L01+, and CC1101 share SPI2. The firmware initializes the
bus, mounts a detected SD card, and only then probes the radio modules; if a
detected card cannot be mounted, radio SPI access stays blocked to avoid
clocks reaching a card that has not entered SPI mode.

| Signal | ESP32-C5 GPIO | SPI clock |
|--------|---------------|-----------|
| SCK / MISO / MOSI | 6 / 2 / 7 | Shared bus |
| SD CS / card detect (CD) | 1 / 0 | Up to 20 MHz |
| nRF24 CSN / CE | 9 / 8 | 4 MHz |
| CC1101 CS / GDO0 | 23 / 24 | 2 MHz |

CD is active-low and uses an ESP internal pull-up. Confirm the card socket's
CD switch is wired to ground when a card is inserted. The SD card's CMD and
DAT lines require pull-ups (typically 10 kOhm); the CD pull-up is separate.
GPIO7 is also an ESP32-C5 JTAG-source strapping pin. Its default eFuse
configuration ignores the strap, but if JTAG source selection is enabled by
eFuse, the SD CMD pull-up biases GPIO7 high during reset and can select the
USB Serial/JTAG source. Verify the board's eFuse/debug configuration before
relying on pad-JTAG.
All three CS lines are held high while idle. Keep the SPI wiring short and
let the ESP-IDF SPI/SD drivers generate SCK only for transactions. The SD SPI
driver is configured for up to 20 MHz; Espressif notes that shared-bus loading
can require a lower rate, so verify signal integrity on the assembled hardware.

`sd format` is destructive and requires explicit confirmation in the app. It
can format a detected card even when the existing FAT filesystem cannot be
mounted. `sd cp-to-lfs` and `sd cp-from-lfs` copy files in either direction.
The app downloads SD files to the platform Downloads folder; preview is
limited to 2 KiB. Use `sd unmount` before physically removing a mounted card.
See Espressif's [SD SPI sharing](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c5/api-reference/peripherals/sdspi_share.html),
[SD pull-up requirements](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c5/api-reference/peripherals/sd_pullup_requirements.html),
and [ESP32-C5 boot configuration](https://documentation.espressif.com/esp32-c5_datasheet_en.html#page=36);
the CC1101 command definitions are in the
[TI datasheet](https://www.ti.com/lit/ds/symlink/cc1101.pdf).

### Infrared (universal remote + receiver)

IR runs on the RMT peripheral, independent of the Wi-Fi/BLE/nRF24 radios.
Wire a 38 kHz IR front-end to 3.3 V logic:

| Part                         | ESP32-C5 GPIO |
|------------------------------|---------------|
| IR LED (via NPN/driver), TX  | 4             |
| IR receiver (TSOP/VS1838), RX| 5             |

- **Universal remote** — `ir univ <category> <button>` sweeps the built-in
  codes for that function. The existing 765-entry TV library remains separate
  and unchanged. AC, Projector, LED, Fan, and Audio libraries are converted
  from their Flipper `.ir` assets into separate per-category entry and RAW
  includes; a category's data stays in its own pair of files. Parsed protocols
  are encoded on the fly, and RAW timings retain their source carrier and duty
  cycle. Long durations are split
  across RMT symbols, and the TX buffer accommodates the longest supplied fan
  signal.

  To regenerate the non-TV tables from the SD-card asset directory:

  ```powershell
  python .\tools\generate_ir_universal_db.py "C:\path\to\sdcard\infrared\assets"
  ```

  The source directory must contain `ac.ir`, `projectors.ir`, `leds.ir`,
  `fans.ir`, and `audio.ir`. The converter validates button names, protocols,
  RAW timing lengths, and RMT symbol capacity before writing either include.
- **Receiver** — `ir rx start` captures bursts, decodes NEC/NECext/Samsung32/
  SIRC/RCA where possible, and streams the decoded summary plus the raw µs
  timings. `ir spectrum on` adds a live mark-coverage envelope (48 bins) of
  each burst for the app's spectrum view.
- **Storage** — the last capture is saved with `ir save <name>` to a LittleFS
  `storage` partition in a Flipper-compatible `.ir` file. `ir ls` / `ir info`
  / `ir rm` manage the library; `ir emulate <name>` replays a saved signal
  (loops until `ir emulate stop`). Raw-timing replay works for any remote,
  including ones with no built-in code.

The partition table reserves 6 MB for the app and 10 MB for IR storage; see
`partitions.csv`. The `storage` partition formats itself on first boot.

### Whitelist / blacklist semantics

Whitelist is a hard veto — no frame is sent that targets a whitelisted
MAC, and no AP whose BSSID is whitelisted will be hit. If a whitelisted
STA is associated to the selected AP (per `sniff`), broadcast and
disassoc modes auto-downgrade to unicast against every non-whitelisted
STA.

Blacklist is an always-target overlay. Every attack cycle also fires
unicast deauths at any blacklisted STA visible in the sniff list, and
broadcast deauths at any blacklisted BSSID present in the AP scan list,
regardless of the currently selected `t24/t5/sta`. Whitelist always
beats blacklist for the same MAC.

## Why JTAG flash, not CDC

Stock IDF's flash flow uses esptool over the CDC ACM serial channel
with DTR/RTS-pulse-driven reset. On the XIAO C5, the USB-Serial-JTAG
controller's interpretation of CDC modem control lines is fragile —
pyserial's default `DTR=True` on open pulls BOOT low and traps the
chip in download mode across soft resets, which then cannot be
escaped without a physical EN-pin event. Flashing over JTAG bypasses
all of that: it goes through a separate USB endpoint and never
touches DTR/RTS or the CDC console.

## Why we don't use PlatformIO

Tried, doesn't work for this chip. The official `platformio/espressif32`
platform doesn't list `esp32c5` as a board. The
`Seeed-Studio/platform-seeedboards` package does ship a
`seeed-xiao-esp32-c5.json`, but its `espidf` framework builder
(`builder/frameworks/espidf.py`) imports two helper modules
(`component_manager.py`, `penv_setup.py`) that are referenced but
not present in the package, so the build fails before compilation
starts. Falling back to the official ESP-IDF VS Code extension is
the supported path.

## Status

- Serial CLI: working
- BLE GATT (NimBLE NUS): initialized by `main.c`; advertises the NUS UUID
  in the primary advertisement and the full `Radio C5` name in
  the scan response. The Flutter app filters by the NUS UUID and lists
  the device before connecting.
- Notifications are split to fit each link's negotiated MTU. The device
  remains connected while idle; BLE supervision handles radio link loss,
  and the app reports disconnects and failed writes instead of hiding them.
- BLE CLI writes are acknowledged, serialized, and reassembled per connection
  until newline, so commands remain intact even when the phone uses the
  default 23-byte ATT MTU.
- Hardware validation still requires an ESP32-C5 and a phone: confirm the
  device appears in the app's scan list, connect, receive the initial status
  and list output, leave it idle for more than one minute, and verify that it
  remains connected before disconnecting and reconnecting.
