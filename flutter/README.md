# ESP32-C5 BLE controller

The Flutter app discovers the firmware as a Nordic UART Service (NUS)
peripheral, displays matching devices in the scan list, and connects only
after the user selects a device. It subscribes to the NUS TX notifications
before sending the initial CLI commands.

The Home dashboard displays the ESP32-C5's internal die-temperature sensor,
sampled four times per second and streamed over BLE. For the configured
20–100 °C range, ESP-IDF reports a sensor measurement error below 2 °C; the
display is not ambient temperature or a precision-calibrated measurement.

The dashboard's CPU load is calculated from FreeRTOS idle-task runtime as a
rolling one-second utilization average, refreshed four times per second. It is
not a frame rate or CPU clock-frequency reading.

## Apple AirTag-format monitor

Open **Apple AirTag Monitor** from Home to start a passive, continuous scan
through the ESP32-C5 BLE wardriving engine. The results list is filtered to
Apple manufacturer advertisements (`0x004C`, subtype `0x12`) that also match
the known 29-byte AirTag lost-mode payload shape (`length byte 0x19`). The
general BLE wardriving list and its all-device reports are not filtered by this
additional test. The monitor shows observation counts and RSSI updates at
approximately two-second intervals.

These are AirTag-format candidates, not authenticated AirTags. BLE alone cannot
prove that a matching payload came from an original AirTag rather than a
compatible accessory or a device replaying the same public format. BLE
addresses can rotate, RSSI is signal strength rather than an accurate distance,
and the advertisements do not provide the tracker owner's identity or location.
The monitor therefore shows the current BLE address and explicitly labels
tracker location and accurate distance as unavailable.

Use **Save to LittleFS** in a live candidate monitor to append a snapshot to
`findmy_observations.csv` on the ESP32-C5. The record contains device uptime,
the current BLE address, AirTag-format classification, RSSI,
received-advertisement count, and any broadcast name. It does not contain phone
GPS location.

## Android permissions

- Android 12 (API 31)+: allow **Nearby devices** (Bluetooth scan and connect).
- Android 6–11 (API 23–30): allow location while using the app and enable the
  phone's Location service; Android requires both to scan for BLE peripherals.

The app automatically resumes scanning after a scan cycle ends. Use the scan
button to stop or restart scanning. Connection and write failures are shown
in the device screen; a lost link requires returning to the scan list and
connecting again.

## Build and test

From this directory, run:

```sh
flutter pub get
flutter analyze
flutter test
flutter build apk --release
```

The test/build checks cannot verify radio behavior. On hardware, confirm the
`Radio C5` device appears in the scan list, connect and verify CLI
output arrives, leave the connection idle for over a minute, then test
disconnect and reconnect.
