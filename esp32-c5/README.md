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
    ├── nrf24.{c,h}             nRF24L01+ SPI text link
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
nrf24 status                  show nRF24 radio status
nrf24 send <text>             send text (maximum 31 bytes)
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
reset                         soft-reset the chip (esp_restart)
```

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

The firmware listens continuously and accepts text transmissions through
`nrf24 send`. Payloads are fixed-width on air, with a 31-byte text limit.
The peer must use channel 76, 1 Mbps, the 5-byte address
`E7:D3:F4:A1:B2`, and auto-ack on pipe 0. Received messages are printed to the
serial/BLE log transports. `nrf24 status` reports whether initialization
succeeded.

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
