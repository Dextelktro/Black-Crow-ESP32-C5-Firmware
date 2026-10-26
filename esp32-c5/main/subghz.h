#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "subghz_presets.h"

/* ============================================================
 * Sub-GHz feature orchestrator (CC1101). Stage 1: radio bring-up, frequency
 * and preset configuration, and a one-shot status/RSSI probe. Capture, TX,
 * analyzer, spectrum, store and protocol decoding are layered on in later
 * stages. One radio session is active at a time; the CC1101 is kept powered
 * down between sessions.
 * ============================================================ */

esp_err_t subghz_init(void);
bool      subghz_module_ready(void);

/* Current (pending) configuration — applied to the chip on the next session. */
uint32_t  subghz_get_frequency(void);
subghz_preset_id_t subghz_get_preset(void);

/* Validate against the Flipper Sub-GHz band limits (300/315/390/433/868/915
 * regions). */
bool      subghz_frequency_valid(uint32_t hz);

/* Store configuration (validated). Does not touch the radio if idle. */
esp_err_t subghz_set_frequency(uint32_t hz);
esp_err_t subghz_set_preset(const char *name);

/* Wake the radio, apply the current preset + frequency, read chip identity and
 * a live RSSI sample, emit a `subghz: STATUS ...` telemetry line, return the
 * chip to sleep. Refused while a sweep (analyzer/spectrum) is active. */
esp_err_t subghz_status(void);

/* ---- Stage 2: RSSI-sweep modes (one active at a time, own worker task) ---- */

/* True while any Sub-GHz worker mode is running. */
bool subghz_is_busy(void);

/* Frequency Analyzer: hop a fixed list across all bands, report the strongest
 * frequency each cycle via `subghz: ANALYZER peakfq=<hz> rssi=<dBm>`. */
esp_err_t subghz_analyzer_start(void);
esp_err_t subghz_analyzer_stop(void);

/* Spectrum: sweep `bins` RSSI samples across `center ± span/2`, emitting a
 * `subghz: SPEC ...` header then chunked `subghz: SPECBIN ...` lines per sweep
 * (fed to the app waterfall). center/span in Hz; pass 0 to use defaults
 * (current frequency, 2 MHz span, 64 bins). */
esp_err_t subghz_spectrum_start(uint32_t center_hz, uint32_t span_hz, int bins);
esp_err_t subghz_spectrum_stop(void);

/* Focused spectrum bands the UI offers (1-based index). The sweep concentrates
 * all bins inside the chosen narrow band so the user can zoom one signal
 * region instead of a whole ISM band. */
#define SUBGHZ_SPEC_BAND_COUNT 6

/* Fetch band `index` (1..SUBGHZ_SPEC_BAND_COUNT) metadata. Returns false if the
 * index is out of range. Any out-pointer may be NULL. */
bool subghz_spectrum_band(int index, const char **label,
                          uint32_t *lo_hz, uint32_t *hi_hz);

/* Start a focused spectrum sweep on band `index` (1-based). bins<=0 => default. */
esp_err_t subghz_spectrum_start_band(int index, int bins);

/* Emit the band table as `subghz: SPECBANDS ...` / `subghz: SPECBAND ...`
 * telemetry so the app can populate its band picker from firmware. */
void subghz_spectrum_list_bands(void);

/* ---- Stage 3: Read RAW (async OOK edge capture + RSSI-qualified capture) ---- */

/* Start/stop continuous raw capture. Captured bursts with a recent above-
 * threshold RSSI sample are streamed as `subghz: RAW ...` + chunked `subghz:
 * RAWBIN ...` and kept as the last capture (for save/emulate). Strict mode
 * accepts the first complete waveform immediately and suppresses similar
 * duplicates; it does not require a second transmission. Default mode also
 * preserves truncated captures for recovery. Reported `fq` is the verified
 * receiver tune, not an independent measurement of the transmitter's carrier
 * frequency. */
esp_err_t subghz_readraw_start(bool strict);
esp_err_t subghz_readraw_stop(void);

/* RAW RSSI threshold in dBm: the above-threshold state is held briefly to cover
 * RMT idle-stop/callback latency. Read mode always runs protocol validation
 * regardless of RSSI. Default -90 dBm (Flipper Unleashed RAW default). */
void subghz_set_rssi_threshold(int dbm);
int  subghz_get_rssi_threshold(void);

/* Emit a `subghz: RAWSTATS ...` line of capture/squelch diagnostic counters
 * (symbols received, truncations, buffer overflows, re-arms, callback latency,
 * squelch opens, RSSI read errors) — to verify no data loss without per-edge
 * logging. */
void subghz_readraw_stats(void);

/* ---- Stage 4: save the last capture (RAW or decoded key) to a .sub file ---- */

/* Save a snapshot of the most recent capture to the selected storage target.
 * READ and READ_RAW may remain active while saving; other busy modes are
 * refused. Emits NOCAP if nothing has been captured, no-storage-target if the
 * target is unset, or sd-absent if SD is selected but missing. */
esp_err_t subghz_save(const char *name);

/* Print one stored file's kind + fields (RAW: n/freq/preset; KEY:
 * proto/bits/key/te/freq/preset). Refused while a sweep/capture/TX is active. */
void subghz_info(const char *name);

/* ---- Stage 5: transmit / emulate (OOK via GPTimer) ---- */

/* Transmit the last captured RAW burst `repeats` times (0 => default). */
esp_err_t subghz_tx_last(int repeats);

/* Load a saved .sub and transmit it `repeats` times (0 => default). Uses the
 * file's own frequency + preset. */
esp_err_t subghz_emulate(const char *name, int repeats);

/* ---- Stage 6: Read (decode known protocols) ---- */

/* Capture + decode: each complete burst is run through the protocol registry.
 * One protocol-valid frame is enough to emit `subghz: READ proto=..`; an
 * undecoded complete burst is shown as `proto=unknown` only when RSSI-qualified.
 * The raw burst is kept for save. */
esp_err_t subghz_read_start(void);
esp_err_t subghz_read_stop(void);

/* ---- Stage 6b: Brute force (iterate a protocol's code space, TX each) ---- */

/* Start brute-forcing `proto_name` with `bits` data bits (<= 20), each code
 * sent `repeats` times (0 => default). Iterates 0..2^bits-1 at the current
 * frequency with the OOK preset. */
esp_err_t subghz_brute_start(const char *proto_name, int bits, int repeats);
esp_err_t subghz_brute_resume(void);   /* continue from the saved index */
esp_err_t subghz_brute_stop(void);

/* Stop whichever Sub-GHz mode is active (idempotent). TX frames are aborted.
 * Success means the worker completed radio/GPIO cleanup; a timeout means the
 * worker may still be finishing cleanup. */
esp_err_t subghz_stop(void);
