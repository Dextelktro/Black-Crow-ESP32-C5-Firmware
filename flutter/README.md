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

## Installing the Android APK (sideload)

The app ships as a single universal APK (`app-release.apk`) that runs on every
Android CPU architecture (arm64-v8a, armeabi-v7a, x86_64) and on Android 7.0
through the target SDK. It is **not** on Google Play, so Android and some
vendors treat it as an "unknown source" and may block the install even though
the APK is a normal, signed build. If an install reaches 100% and then reports
that it failed, it was **blocked, not corrupted**. Work through these steps:

1. **Uninstall any older build first.** This APK is signed with the project's
   permanent release key. A copy signed with a different key (an older release,
   or a build from another source that used the same package name) cannot be
   updated in place, and the install fails at the end. Uninstalling deletes
   that app's local data.
2. **Allow your file manager / browser to install apps.** Settings → Apps →
   (the app you open the APK from, e.g. *My Files* or *Chrome*) → *Install
   unknown apps* → allow.
3. **Samsung only — turn off Auto Blocker.** One UI 6.1.1 and newer ship with
   *Auto Blocker* on by default, and it blocks all sideloading even after
   step 2. Settings → Security and privacy → Auto Blocker → off (it may ask for
   your PIN or fingerprint).
4. **If Play Protect warns or blocks**, choose *Install anyway* (sometimes under
   *More details*). Play Protect flags apps it does not recognise; it is not
   reporting a detected virus.

To find the exact reason a single install failed, run
`adb logcat | grep -i PackageManager` while installing; the `INSTALL_FAILED_…`
line names the cause.

### Bahasa Indonesia — memasang APK (sideload)

Aplikasi dibagikan sebagai satu APK universal (`app-release.apk`) yang jalan di
semua arsitektur (arm64-v8a, armeabi-v7a, x86_64) dan Android 7.0 ke atas.
Aplikasi ini **tidak** ada di Google Play, jadi Android dan sebagian vendor
menganggapnya "sumber tidak dikenal" dan bisa memblokir pemasangan walau APK-nya
build normal yang sudah ditandatangani. Kalau instalasi sampai 100% lalu muncul
"gagal", berarti **diblokir, bukan file rusak**. Ikuti langkah ini:

1. **Copot dulu versi lama.** APK ini ditandatangani dengan kunci rilis tetap
   proyek. Versi lama dengan kunci berbeda (atau build dari sumber lain yang
   memakai nama package sama) tidak bisa di-update di tempat, jadi instalasi
   gagal di akhir. Mencopot akan menghapus data lokal app lama.
2. **Izinkan file manager/browser memasang app.** Setelan → Aplikasi → (app
   tempat kamu membuka APK, mis. *My Files* atau *Chrome*) → *Pasang aplikasi
   tidak dikenal* → izinkan.
3. **Khusus Samsung — matikan Auto Blocker.** One UI 6.1.1+ mengaktifkan *Auto
   Blocker* secara default, dan itu memblokir semua sideload walau langkah 2
   sudah dilakukan. Setelan → Keamanan dan privasi → Auto Blocker → matikan
   (mungkin minta PIN/sidik jari).
4. **Jika Play Protect memperingatkan atau memblokir**, pilih *Tetap pasang*
   (kadang di bawah *Detail lainnya*). Play Protect menandai app yang tak
   dikenal; ini bukan berarti virus terdeteksi.

## Install with Obtainium (recommended — auto-updates)

[Obtainium](https://github.com/ImranR98/Obtainium) installs this app straight
from the project's GitHub Releases and keeps it updated automatically — no app
store and no manual re-downloads.

[![Add to Obtainium](https://img.shields.io/badge/Obtainium-Add%20app-7A2BE2?logo=android&logoColor=white)](https://apps.obtainium.imranr.dev/redirect.html?r=obtainium://add/https://github.com/davidlana971-ui/Black-Crow-ESP32-Firmware)

1. Install **Obtainium** (from its own GitHub Releases or F-Droid).
2. Tap the badge above, or in Obtainium tap **Add App** and paste the repo URL:
   `https://github.com/davidlana971-ui/Black-Crow-ESP32-Firmware`
3. Tap **Add**, then **Install**. The one-time OS steps above (allow *Install
   unknown apps*, and on Samsung turn off *Auto Blocker*) still apply the first
   time only.

Obtainium then notifies you and updates the app whenever a new release is
published.

### Bahasa Indonesia — pasang via Obtainium (disarankan, auto-update)

[Obtainium](https://github.com/ImranR98/Obtainium) memasang app ini langsung
dari GitHub Releases proyek dan meng-update otomatis — tanpa toko aplikasi dan
tanpa unduh ulang manual.

1. Pasang **Obtainium** (dari GitHub Releases-nya sendiri atau F-Droid).
2. Ketuk badge di atas, atau di Obtainium ketuk **Add App** lalu tempel URL
   repo: `https://github.com/davidlana971-ui/Black-Crow-ESP32-Firmware`
3. Ketuk **Add** lalu **Install**. Langkah OS sekali-jalan di atas (izinkan
   *Pasang aplikasi tidak dikenal*, dan di Samsung matikan *Auto Blocker*) tetap
   berlaku hanya saat pertama kali.

Setelah itu Obtainium memberi notifikasi dan meng-update app setiap ada rilis
baru.

## Build and test

From this directory, run:

```sh
flutter pub get
flutter analyze
flutter test
flutter build apk --release
```

Android release APKs must use the permanent release keystore; release builds
never fall back to the debug key. For local release builds, configure
`android/key.properties` with the same keystore used for published releases.
The key file and properties file are ignored by Git and must never be committed.

The GitHub Actions release workflow requires these repository secrets:
`KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_PASSWORD`, and `KEY_ALIAS`. It also
requires the repository Actions variable `ANDROID_SIGNING_CERT_SHA256`, the
64-character SHA-256 fingerprint of the release certificate (hexadecimal,
without colons). The workflow verifies that both the keystore and built APK
match this fingerprint before publishing. To create a permanent key once,
from this directory run the following in PowerShell; keep the keystore and an
offline backup safe, and never replace it:

```sh
keytool -genkeypair -v -keystore android/app/blackcrow-release.jks -storetype JKS -keyalg RSA -keysize 2048 -validity 10000 -alias blackcrow
```

Export the public certificate and calculate its fingerprint:

```powershell
keytool -exportcert -keystore android/app/blackcrow-release.jks -storetype JKS -alias blackcrow -file android/app/blackcrow-release-cert.der
(Get-FileHash android/app/blackcrow-release-cert.der -Algorithm SHA256).Hash
```

Set the hash output as the Actions variable, and copy the keystore's Base64
value into the `KEYSTORE_BASE64` Actions secret with
`[Convert]::ToBase64String([IO.File]::ReadAllBytes("android/app/blackcrow-release.jks")) | Set-Clipboard`.
Paste it directly into the secret field, then clear the clipboard. Set the
other three secrets to the keystore password, key password, and alias, and
create `android/key.properties` locally with those values and
`storeFile=../app/blackcrow-release.jks`. Never commit the keystore or properties
file. Replacing the permanent key changes the Android app identity and
prevents normal in-place updates.

The generated APK is checked for `minSdkVersion` 24 (Android 7.0), target SDK
36 or newer, ARM64/ARMv7/x86_64 native libraries, and APK Signature Scheme v2.
Android 7.0 introduced v2 signing, so it is compatible with the app's API 24
minimum. The signing certificate is not device- or architecture-specific;
compatibility is determined by the APK's SDK range, ABI contents, and Android's
signing scheme support. This validation covers Android 7 through the target SDK
in the artifact; it cannot guarantee every vendor-specific firmware or future
Android release without testing on those devices.

Release tags use the format `BCfw-v<firmware-version>_app-v<app-version>`,
for example `BCfw-v0.1.0_app-v1.0.0+1`. The same tag pattern triggers the
firmware, Android, Linux, and GitHub Release jobs. Tags pushed before this
workflow is updated are not replayed automatically; create a new matching tag
after configuring the signing secrets and fingerprint variable.

The first release signed with the permanent key cannot normally update older
installations that were signed with the previous debug key. Those users may
need to uninstall the old app before installing the new one; uninstalling
deletes local app data, so back up anything important first.

The test/build checks cannot verify radio behavior. On hardware, confirm the
`Radio C5` device appears in the scan list, connect and verify CLI
output arrives, leave the connection idle for over a minute, then test
disconnect and reconnect.
