# Sub-GHz logic tests (host, no hardware)

These host-run Python tests validate the **logic** of the Sub-GHz protocol and
storage code before an RF module is available. They do **not** run the compiled
firmware; each test mirrors the C in `esp32-c5/main/subghz_protocol.c` /
`subghz_store.c` / the Stage 6b brute worker, and drives it with waveforms /
files built **directly from the Flipper spec** (independent reference vectors,
not our own encoder — so a shared encoder/decoder bug cannot hide).

Run:

    python esp32-c5/tools/subghz_tests/test_subghz_protocol.py   # decode/encode + encoder bounds
    python esp32-c5/tools/subghz_tests/test_subghz_logic.py      # brute + RAW parser
    python esp32-c5/tools/subghz_tests/test_subghz_keyfile.py    # protocol Key File
    python esp32-c5/tools/subghz_tests/test_subghz_pipeline.py   # capture pipeline (RMT seg + normalize -> decode)

Latest host run: 440 PASS across the four scripts below, 0 FAIL.

## Capture boundary (Stage 7 Part C)

Two SEPARATE concepts, not one global threshold:
- **Hardware** `SUBGHZ_RX_MAX_NS` (10 ms) = the longest level the RMT records
  before it ends a reception (ESP-IDF `signal_range_max_ns` semantics). Must
  exceed the longest in-frame pulse; kept at 10 ms so the ten existing protocols
  segment exactly as before.
- **Software** `SUBGHZ_FRAME_GAP_US` (6 ms) = frame-end. Within one reception a
  low ≥ 6 ms splits the stream into frames. This resolves short-gap protocols
  (SMC5326 ~7.2 ms, Dooya ~9.2 ms) whose inter-frame gap is below the 10 ms
  hardware limit — the gap is recorded inside the reception and the software
  separates the frames. Invariant: longest in-frame pulse (Clemsa 2695 µs) <
  6000 µs < shortest inter-frame gap (SMC5326 ~7200 µs).

`test_subghz_pipeline.py` proves, against a mirror of the RMT stop semantics:
long headers do not truncate frame data; a 7-10 ms gap splits while a 2.7 ms
valid pulse and a 5.999 ms low do not; many frames in one reception split (with
distinct keys recovered in order — no loss/dup); a partial frame never decodes;
Dooya/SMC5326 short-gap segmentation; and all 10 protocols still decode one
frame per reception (regression). It also covers the 3 us ESP32-C5 RMT
glitch-filter boundary, decoder-only repair of isolated sub-50 us spikes,
two-frame Read confirmation, optional strict-RAW waveform matching,
duplicate suppression, the delayed RSSI qualification hold, and median-of-three
RSSI sampling. The test reads the relevant firmware `#define`s so capture
thresholds, confirmation windows, and sample counts cannot silently drift
from the model. Analyzer checks cover the user-adjustable detection trigger and
the start-time auto-calibration that sets it a fixed margin above the measured
band noise floor (the old fixed absolute RSSI floor and sideband margin are
gone).

## Verified now (logic)

- Decode of Princeton / Linear / Clemsa / SMC5326 / Dooya / Magellan /
  Intertechno_V3 / CAME / Nice FLO / Holtek / Gate-TX / Doitrand / Phoenix_V2 (AM)
  and Ansonic (FM) from independent synthetic captures → correct protocol, key and
  bit count (incl. 18-, 25-, 32-, 37-, 40- and 52-bit keys). Intertechno_V3
  (32-bit) is bespoke: a short-high + te_short*10 (2750 us) sync, then 32 bits each
  sent as TWO (short H, low) pairs — the first low sets the bit (short=0, long=1)
  and the second is its complement. The ~10.4 ms inter-frame gap gives each frame
  its own reception; the capture trims the final bit's complement low (a trailing
  low) so a frame can end on the 32nd bit's second high, which the decoder accepts
  (the bit value is already fixed by the first low). The 36-bit dimming sub-variant
  is NOT supported (such a frame is rejected, not mis-decoded). Magellan (32-bit) is a bespoke decoder: a
  sync-pulse train (>10 short/short pairs), a (short,long) marker, a (te_short*6,
  long) start bit, 32 HIGH-first 2:1 bits (inverted vs Dooya), a lone stop high,
  and a CRC-8/0x31 over the upper 3 bytes that gates acceptance (so it runs last
  and cannot false-trigger; a corrupted CRC or <11 sync pairs is rejected).
  Clemsa/SMC5326/Dooya share the HIGH-first helper with
  Linear (7:1, 3:1, 2:1 and 3:1 ratios). SMC5326 (25-bit) adds a thin wrapper for
  its trailing stop high and is dispatched before Princeton (its exact-25 +
  whole-frame consume keeps a Princeton-24 frame from being stolen, and
  vice-versa); its encoder reuses encode_princeton with a te*25 guard. Dooya
  (40-bit) gates its start-high (te_short*13 = 4758 us, ±te_delta*5) and start-low
  (te_long*2 = 1466 us, ±te_delta*3) preamble — the start low sits above an
  ordinary long leg (733 + te_delta*2 = 973), so no data pair can satisfy it —
  then decodes 40 HIGH-first 2:1 bits with a te_delta*2 long-leg window; the 40th
  bit's low merges into the next frame's ~9 ms header, so it arrives as a lone
  trailing high.
- Timing tolerance: CAME holds ±20%; Princeton ±5–10% (tighter — discriminated
  by the 3:1 ratio); Ansonic ±10% (its te_delta=120 on te_long=1111 is the
  faithful edge — Flipper rejects beyond it too); Gate-TX ±10% (short-leg
  window is ±100 on 350).
- Modulation-aware decode: Ansonic (FM/2FSK) and the AM/OOK protocols are
  attempted only for a capture of their own modulation, exactly as Flipper
  registers protocols per-modulation. This removes the Nice FLO ↔ Ansonic
  timing overlap: the identical Ansonic waveform does NOT decode under an AM
  capture, and is never mis-labelled Nice FLO in the jitter band. The capture
  modulation comes from an explicit per-preset `fm` flag, not from enum order.
- Capture pipeline end-to-end (`test_subghz_pipeline.py`): a FULL on-air stream
  of several frame repeats WITH their 17-21 ms inter-frame header/trailer gaps
  is run through mirrors of the RMT segmenter (`signal_range_max_ns` = 10 ms)
  and `symbols_to_durations()` normalizer, then decoded. Proves the headers
  (longer than the 10 ms stop-threshold, as they must be) correctly SEGMENT the
  stream into one decodable burst per frame — rather than fusing frames or
  truncating one — and that the longest intra-frame low (≤1.5 ms) never splits a
  frame. Holds under ±5-10% jitter; a CAME control proves the path is generic.
- Emulate modulation guard: a Key File whose protocol modulation disagrees with
  its stored preset (e.g. FM Ansonic under an OOK preset, or an AM protocol
  under 2FSK) is refused BEFORE transmit; a key wider than its declared bit
  count is refused. Brute likewise only accepts small OOK code spaces.
- Cross-protocol separation: Linear is not stolen by Princeton/Nice (whole-frame
  consume + exact 10-bit); Gate-TX vs CAME kept apart by the start-bit window
  (te_long vs te_short); each new protocol classifies as itself.
- False-positive resistance: CAME-24 is NOT misread as Princeton (the ratio
  fix); 50000 random alternating bursts → 0 false decodes in BOTH the AM and
  FM protocol sets; unsupported bit counts / wrong Holtek header nibble rejected.
- Encoder output equals the independent spec frame (level + duration sequence),
  MSB-first bit order (0x800001 vectors), header/guard placement.
- Decoder is stateless: valid → junk → valid decodes identically.
- Brute worker index: resume = "next code to send"; stop+resume covers every
  code exactly once (no skip/dup); bits > 20 rejected (so Princeton-24 and
  Holtek-40 cannot brute); progress-64 boundary has no miscount.
- `.sub` parser is STRICT: missing/zero Frequency, non-numeric/zero/over-range
  tokens, < 2 values, and more-than-cap tokens all REJECT the file (no silent
  skip or truncation); multiple `RAW_Data:` lines accumulate in order.

## Pending hardware (cannot be proven without an RF module)

- Real remote frames actually decode (true pulse timing vs our windows).
- Interop with a genuine Flipper (import/export `.sub`).
- TX pulse accuracy on GDO0 (scope/logic-analyzer), minimum reproducible pulse,
  cumulative drift over a long frame, jitter under WiFi/BLE/flash load.
- RSSI threshold vs real noise floor and partial-burst behavior. The hardware
  RMT glitch floor remains 3 us: ESP-IDF 5.5.1's ESP32-C5 APB-clock filter is
  limited to 255 ticks (about 3187 ns). A separate software repair merges only
  isolated sub-50 us glitches for protocol decoding; the caller-owned RAW
  waveform remains unchanged. Read reports a single complete frame as soon as
  a supported protocol decoder accepts it, then suppresses identical reports
  for 1.5 seconds. Complete undecoded frames are shown as Unknown only while
  RSSI-qualified. Strict RAW emits the first complete qualifying waveform
  immediately, rejects truncated captures, and suppresses similar duplicates
  for 1.5 seconds; it never waits for a second transmission. The RAW RSSI gate
  uses a 100 ms last-above hold and defaults to -90 dBm, matching Flipper
  Unleashed. The analyzer has no fixed absolute floor: at start it auto-
  calibrates a detection trigger SUBGHZ_ANALYZER_AUTOCAL_MARGIN_DB above the
  band noise floor it measures, and the user can override that trigger from the
  app (clamped to the -110..0 dBm firmware range). These are logic-tested, but
  threshold calibration and false-detection rates still require RF hardware
  measurements.
  The "header gap > 10 ms stop-threshold segments frames" assumption is proven
  at the logic level; real on-air pulse timing remains hardware-pending.
- Ansonic/2FSK TX actually demodulating on a real FM receiver (the GPTimer path
  keys GDO0; the CC1101 preset decides OOK vs 2FSK — unverified on hardware).
- Brute force real RF emission (test only on your own / authorized equipment,
  RF-attenuated or isolated).
- Radio stability over repeated start/stop of every mode.

## Known limitations (logic)

- Read decode only recognizes the protocol implementations in the firmware.
  An unsupported remote can appear as RSSI-qualified Unknown, but cannot be
  decoded into a key without adding its protocol. A protocol-valid frame is
  strong format evidence, not source authentication.
- Read RAW is intentionally broader than Read decode. RSSI qualification and
  edge-count checks reduce idle/noise captures, but cannot prove a waveform
  came from a particular remote. Strict mode does not wait for repeats; it
  rejects truncated captures and deduplicates after the first report.
- Read's displayed frequency is the CC1101 tuning setting. Spectrum and
  Frequency Analyzer estimate an RSSI peak from discrete tuning steps; they
  are not independent carrier-identification measurements. A narrower CC1101
  channel filter can reduce adjacent-channel energy, but cannot authenticate
  the source or create a brick-wall channel boundary.
- Save writes a protocol Key File when the last capture decoded, else a RAW
  file; emulate re-encodes a Key File or replays a RAW file.
- Linear: Flipper stores the raw data bits in the `.sub` (what we store too) but
  DISPLAYS `~data` for historical remote compatibility. Matching that inverted
  display is an app-layer concern; the stored/encoded value is the raw bits.
- Holtek_HT12X (12-bit): on-air timing (ts 320 / tl 640) is identical to CAME-12
  with no distinguishing header/checksum, and the only difference (preamble
  length) is stripped by capture — so a single frame cannot be told apart. Handled
  as a NAME ALIAS to the CAME codec (like EV1527→Princeton), NOT a separate
  decoder; decode/save report "CAME".
- Deferred protocols (need more than fixed-window single-burst decode):
  - Chamberlain_code: bespoke trinary (4-bit packed symbols STOP/0/1, ~4 ms
    pulse-position pairs, three dip variants 7/8/9 with mask repacking and a
    variable-width key, plus a bit-array encoder) — implementable but deserves a
    focused step; not yet done.
  - Intertechno_V3 36-bit dimming sub-variant (the 32-bit variant is supported).
  - Marantec: Manchester encoding (not PWM) and a header of exactly ~10 ms.
  - Megacode: bespoke framing (pulse-position slot with in-bit low gaps up to
    ~8 ms, which exceed the 6 ms frame-gap and collide with SMC5326's 7.2 ms
    inter-frame gap — no single threshold serves both) — dropped for whole-frame.
