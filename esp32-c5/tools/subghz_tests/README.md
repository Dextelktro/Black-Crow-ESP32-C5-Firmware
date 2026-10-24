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

Last result: `results.log` — 245 PASS, 0 FAIL.

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
frame per reception (regression).

## Verified now (logic)

- Decode of Princeton / Linear / Clemsa / CAME / Nice FLO / Holtek / Gate-TX /
  Doitrand / Phoenix_V2 (AM) and Ansonic (FM) from independent synthetic
  captures → correct protocol, key and bit count (incl. 18-, 37- and 52-bit
  keys). Clemsa shares the HIGH-first helper with Linear (7:1 vs 3:1 ratio).
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
- RSSI squelch threshold vs real noise floor and partial-burst behavior. (The
  "header gap > 10 ms stop-threshold segments frames" assumption is now proven
  at the LOGIC level by `test_subghz_pipeline.py`; only the real on-air pulse
  timing vs the 10 ms threshold remains hardware-pending.)
- Ansonic/2FSK TX actually demodulating on a real FM receiver (the GPTimer path
  keys GDO0; the CC1101 preset decides OOK vs 2FSK — unverified on hardware).
- Brute force real RF emission (test only on your own / authorized equipment,
  RF-attenuated or isolated).
- Radio stability over repeated start/stop of every mode.

## Known limitations (logic)

- Princeton fires on a single frame + 3:1 ratio + Te plausibility. Flipper
  additionally requires two identical frames; if real-world false positives
  appear, add a two-frame confirm (needs cross-burst state) or a Te-consistency
  check. Empirically 0/20000 random false here.
- Save writes a protocol Key File when the last capture decoded, else a RAW
  file; emulate re-encodes a Key File or replays a RAW file.
- Linear: Flipper stores the raw data bits in the `.sub` (what we store too) but
  DISPLAYS `~data` for historical remote compatibility. Matching that inverted
  display is an app-layer concern; the stored/encoded value is the raw bits.
- Deferred protocols (need more than fixed-window single-burst decode):
  - SMC5326: inter-frame guard is only ~7.2 ms, BELOW the 10 ms RMT stop
    threshold, so consecutive frames would not segment; needs a capture-threshold
    decision (and runtime TE + two-frame confirm) before it can be added safely.
  - Holtek_HT12X (12-bit): on-air timing (ts 320 / tl 640) is identical to
    CAME-12, so it cannot be distinguished from timing alone — would just be a
    dispatch coin-flip with CAME.
  - Intertechno_V3: bespoke framing (two H-L pairs per bit, 5:1 ratio, dimming
    symbol) — not a PWM 2:1/3:1 template.
  - Dooya: header 9.16-9.53 ms, below the 10 ms stop-threshold (same short-guard
    class as SMC5326).
  - Marantec: Manchester encoding (not PWM) and a header of exactly ~10 ms.
  - Megacode / Magellan: bespoke framing (6 ms pulse-position slot; short-pulse
    preamble + CRC8) — would need dedicated decoders, deferred.
