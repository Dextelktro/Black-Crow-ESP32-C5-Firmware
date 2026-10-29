#!/usr/bin/env python3
# End-to-end CAPTURE PIPELINE test (host, no hardware).
#
# The other suites feed the decoder a pre-trimmed burst. This one proves the
# step the audit flagged (P0): a FULL on-air stream — multiple frame repeats
# WITH their 17-21 ms inter-frame header/trailer gaps — is segmented by the RMT
# stop-threshold and normalized by symbols_to_durations into exactly the
# decoder-ready bursts the decoder expects. If the 10 ms stop-threshold were
# mis-sized against the real protocol headers, this is where it would break.
#
# Mirrors of the C: subghz_capture.c signal_range_max_ns / signal_range_min_ns
# segmentation + symbols_to_durations() normalizer, and the protocol decoder.
# On-air streams are built DIRECTLY from the Flipper encoding rules (headers
# included), independent of our own encoder.

import re
from pathlib import Path

FAIL = 0
def check(cond, msg):
    global FAIL
    print(f"  {'PASS' if cond else 'FAIL'} {msg}")
    if not cond: FAIL += 1

ROOT = Path(__file__).resolve().parents[2]
CAPTURE_SOURCE = (ROOT / "main" / "subghz_capture.c").read_text(encoding="utf-8")
SUBGHZ_SOURCE = (ROOT / "main" / "subghz.c").read_text(encoding="utf-8")
PROTOCOL_SOURCE = (ROOT / "main" / "subghz_protocol.c").read_text(encoding="utf-8")

def c_define(source, name):
    match = re.search(rf"^#define\s+{name}\s+\(?(-?\d+)\)?", source, re.MULTILINE)
    return int(match.group(1)) if match else None

def median3(a, b, c):
    return sorted((a, b, c))[1]

def rssi_recently_qualified(seen_above, last_above_ms, now_ms,
                            hold_ms=400, threshold=-90):
    if threshold <= -127:
        return True
    return seen_above and now_ms - last_above_ms <= hold_ms

def analyzer_accept(coarse_peak, fine_peak, trigger=None):
    """Mirror the Flipper-faithful analyzer gate (analyzer_scan in subghz.c): a
    detection is reported only when the COARSE peak beats the user trigger AND
    the narrow FINE peak also beats it. There is no fixed absolute threshold and
    no relative noise-floor margin — the trigger is the single, user-adjustable
    gate (Flipper's on-screen bar), which is what makes noise rejection robust
    across hardware. `trigger` defaults to the firmware default."""
    if trigger is None:
        trigger = c_define(SUBGHZ_SOURCE, "SUBGHZ_ANALYZER_DEFAULT_TRIGGER")
    return coarse_peak > trigger and fine_peak > trigger

def repair_short_glitches(durations, min_us=50):
    """Mirror the decoder-only cleanup: absorb a short interior pulse between
    two longer intervals of the same level while preserving elapsed time."""
    out = list(durations)
    i = 1
    while i + 1 < len(out):
        before, glitch, after = out[i - 1:i + 2]
        if (abs(glitch) < min_us and abs(before) >= min_us and
                abs(after) >= min_us and (before < 0) == (after < 0) and
                (before < 0) != (glitch < 0)):
            merged = (1 if before > 0 else -1) * (
                abs(before) + abs(glitch) + abs(after)
            )
            out[i - 1:i + 2] = [merged]
            i = max(1, i - 1)
        else:
            i += 1
    return out

def read_reports(frames, dedupe_ms=1500):
    """Mirror Read's acceptance: a decoded frame is reported on the FIRST decode
    (no repeat confirmation) and identical repeats are suppressed for dedupe_ms.
    Undecoded bursts (None) are NEVER reported — READ lists only genuinely
    decoded signals, so noise never appears (matches Flipper's Read)."""
    last_reported = None
    last_reported_tick = 0
    reports = []
    for decoded, tick in frames:
        if decoded is None:
            reports.append(False)
            continue
        identity = decoded[:3]
        duplicate = (
            identity == last_reported and
            tick - last_reported_tick < dedupe_ms
        )
        if not duplicate:
            last_reported = identity
            last_reported_tick = tick
        reports.append(not duplicate)
    return reports

def raw_timings_match(a, b, tolerance_pct=30, min_tolerance_us=50):
    if len(a) != len(b):
        return False
    for left, right in zip(a, b):
        if (left < 0) != (right < 0):
            return False
        tolerance = max(abs(left) * tolerance_pct // 100, min_tolerance_us)
        if abs(abs(left) - abs(right)) > tolerance:
            return False
    return True

def strict_raw_accepts(durations, truncated):
    return bool(durations) and not truncated

RAWREC_GAP_US = 15000
RAW_MAX = 1024
def rawrec_accumulate(frames, cap=RAW_MAX, gap=RAWREC_GAP_US):
    """Mirror the continuous Read RAW recorder (subghz.c): append each frame into
    ONE recording, inserting a synthesized inter-frame gap before all but the
    first; a frame that cannot fit is dropped whole and truncation is flagged."""
    rec, truncated = [], False
    for f in frames:
        pos = len(rec)
        extra = len(f) + (1 if pos > 0 else 0)
        if pos + extra <= cap:
            if pos > 0:
                rec.append(-gap)
            rec.extend(f)
        else:
            truncated = True
    return rec, truncated

def strictly_alternating(seq):
    return all((seq[i] < 0) != (seq[i + 1] < 0) for i in range(len(seq) - 1))

# ---------- capture pipeline mirror (subghz_capture.c) ----------
RX_MIN_US    = 3        # SUBGHZ_RX_MIN_NS   (C5 APB glitch filter)
RX_MAX_US    = 10000    # SUBGHZ_RX_MAX_NS   (HW: a level > this ends a reception)
FRAME_GAP_US = 6000     # SUBGHZ_FRAME_GAP_US (SW: a low >= this ends a frame)
MIN_EDGES    = 8        # SUBGHZ_RX_MIN_EDGES
DECODE_GLITCH_US = 50

def rmt_segment(stream):
    """HARDWARE layer: split a continuous signed-us stream into receptions the
    way the RMT does — any level longer than RX_MAX_US terminates the current
    reception (recorded as its final symbol, as hardware would, before re-arm).
    A level <= RX_MAX_US does NOT end a reception, so a short inter-frame gap
    stays inside the reception for the software layer to split."""
    recs, cur = [], []
    for v in stream:
        if abs(v) < RX_MIN_US:          # glitch: hardware filters it out
            continue
        cur.append(v)
        if abs(v) > RX_MAX_US:
            recs.append(cur); cur = []
    if cur:
        recs.append(cur)
    return recs

def normalize(rec, despeckle=50):
    """Mirror of symbols_to_durations(): merge consecutive same-sign, drop the
    leading space(s) and the final trailing space, no zeros, and SOFTWARE
    de-speckle — absorb an opposite-level pulse shorter than `despeckle` us
    (SUBGHZ_RX_DESPECKLE_US) into the surrounding level, so OOK-slicer noise the
    3 us RMT glitch filter cannot reach is removed. Interior gaps >= despeckle
    are KEPT (the software splitter needs them)."""
    out = []
    for v in rec:
        if v == 0:
            continue
        if not out:
            if v < 0:
                continue                # drop leading space
            if v < despeckle:
                continue                # drop leading runt mark
            out.append(v)
        elif (out[-1] < 0) == (v < 0):
            out[-1] += v                # same level: merge
        elif abs(v) < despeckle:
            # short opposite spike: absorb into the current level and stay on it;
            # the next same-level fragment merges in above.
            out[-1] += -abs(v) if out[-1] < 0 else abs(v)
        else:
            out.append(v)
    while out and out[-1] < 0:
        out.pop()                       # end on a mark
    return out

def sw_split(nb):
    """SOFTWARE layer: mirror of emit_frames() — split a normalized reception at
    every low of at least FRAME_GAP_US (the gap is a separator), trim each
    segment to start/end on a mark, and keep those with >= MIN_EDGES."""
    frames, start = [], 0
    for i, v in enumerate(nb):
        if v <= -FRAME_GAP_US:
            frames.append(nb[start:i]); start = i + 1
    if start < len(nb):
        frames.append(nb[start:])
    out = []
    for f in frames:
        while f and f[0] < 0:  f = f[1:]
        while f and f[-1] < 0: f = f[:-1]
        if len(f) >= MIN_EDGES:
            out.append(f)
    return out

# ---------- decoder mirror (subghz_protocol.c) ----------
def within(a, t, d):
    return (a - t if a > t else t - a) < d

def decode_phf(d, ts, tl, dsh, dlo, egm, want):   # HIGH-first PWM (Linear/Clemsa)
    data = 0; bits = 0; i = 0; n = len(d)
    while i < n and bits < want:
        hi = d[i]
        if hi <= 0: return None
        long_hi = within(hi, tl, dlo); short_hi = within(hi, ts, dsh)
        if not long_hi and not short_hi: return None
        if i + 1 < n and d[i+1] < 0:
            la = -d[i+1]
            if la < egm:
                long_lo = within(la, tl, dlo); short_lo = within(la, ts, dsh)
                if long_hi and short_lo: bit = 1
                elif short_hi and long_lo: bit = 0
                else: return None
                data = (data << 1) | bit; bits += 1; i += 2; continue
            data = (data << 1) | (1 if long_hi else 0); bits += 1; i += 2; break
        data = (data << 1) | (1 if long_hi else 0); bits += 1; i += 1; break
    if bits != want or i != n: return None
    return (data, bits)

def decode_linear(d):
    r = decode_phf(d, 500, 1500, 350, 350, 2500, 10)
    return ("Linear", r[0] & 0x3FF, 10) if r else None

def decode_smc5326(d):     # 25-bit HIGH-first 3:1 + lone trailing stop high
    if len(d) < 2 or d[-1] <= 0 or not within(d[-1], 300, 200): return None
    r = decode_phf(d[:-1], 300, 900, 200, 200, 1800, 25)
    return ("SMC5326", r[0] & 0x1FFFFFF, 25) if r else None

def decode_dooya(d):       # start H4758 + L1466 preamble, then 40 HIGH-first 2:1 bits
    if len(d) < 4 or d[0] <= 0 or not within(d[0], 4758, 600): return None
    if d[1] >= 0 or not within(-d[1], 1466, 360): return None
    r = decode_phf(d[2:], 366, 733, 120, 240, 2932, 40)
    return ("Dooya", r[0] & 0xFFFFFFFFFF, 40) if r else None

def magellan_crc8(data):   # poly 0x31, init 0x00, MSB-first
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

def decode_magellan(d):    # sync train + marker + start + 32 2:1 bits + stop high + CRC8
    ts, tl, de = 200, 400, 100
    n = len(d); i = 0
    while i < n and not (d[i] > 0 and within(d[i], ts, de)): i += 1
    header = 0; marker = False
    while i + 1 < n:
        if d[i] <= 0 or d[i+1] >= 0: return None
        hi, lo = d[i], -d[i+1]
        if within(hi, ts, de) and within(lo, ts, de): header += 1; i += 2; continue
        if header > 10 and within(hi, ts, de) and within(lo, tl, de*2): i += 2; marker = True; break
        return None
    if not marker or i + 1 >= n: return None
    if d[i] <= 0 or not within(d[i], ts*6, de*3): return None
    if d[i+1] >= 0 or not within(-d[i+1], tl, de*2): return None
    i += 2
    data = 0; bits = 0
    while i + 1 < n and bits < 32:
        if d[i] <= 0 or d[i+1] >= 0: return None
        hi, lo = d[i], -d[i+1]
        if within(hi, ts, de) and within(lo, tl, de): bit = 1
        elif within(hi, tl, de) and within(lo, ts, de): bit = 0
        else: return None
        data = (data << 1) | bit; bits += 1; i += 2
    if bits != 32 or i >= n or d[i] <= 0 or not within(d[i], ts, de) or i + 1 != n: return None
    if (data & 0xFF) != magellan_crc8([(data>>24)&0xFF, (data>>16)&0xFF, (data>>8)&0xFF]): return None
    return ("Magellan", data & 0xFFFFFFFF, 32)

def decode_intertechno(d):   # [short H][2750 L] + 32 bits, two (short H, L) pairs each
    ts, tl, de = 275, 1375, 150
    n = len(d)
    if n < 4 or d[0] <= 0 or not within(d[0], ts, de): return None
    if d[1] >= 0 or not within(-d[1], 2750, de*3): return None
    data = 0; bits = 0; i = 2
    while bits < 32:
        if i + 2 >= n: return None                      # need [H][L1][H]
        if d[i] <= 0 or not within(d[i], ts, de): return None
        if d[i+1] >= 0: return None
        if d[i+2] <= 0 or not within(d[i+2], ts, de): return None
        l1 = -d[i+1]
        if within(l1, tl, de*2): bit = 1
        elif within(l1, ts, de): bit = 0
        else: return None
        data = (data << 1) | bit; bits += 1
        if i + 3 < n and d[i+3] < 0:                     # complement L2 present
            l2 = -d[i+3]
            if (not within(l2, ts, de)) if bit else (not within(l2, tl, de*2)): return None
            i += 4
        elif bits == 32:
            i += 3                                       # final bit: L2 trimmed by capture
        else:
            return None
    if i == n or (i + 1 == n and d[i] > 0 and within(d[i], ts, de)):
        return ("Intertechno_V3", data & 0xFFFFFFFF, 32)
    return None

def decode_princeton(d):   # 24-bit 3:1 H-L pairs (covers EV1527/PT2262/HS1527)
    data = 0; bits = 0; te_ref = 0; i = 0; n = len(d)
    while i + 1 < n and bits < 24:
        hi, lo = d[i], d[i + 1]
        if hi <= 0 or lo >= 0: break
        ha, la = hi, -lo
        mn, mx = min(ha, la), max(ha, la)
        if mn < 80 or mn > 2000: break
        if mx < mn * 2 + mn // 3 or mx > mn * 4: break   # ~3:1 ratio gate
        if te_ref == 0:
            te_ref = mn
        else:
            tol = te_ref // 3
            if mn > te_ref + tol or mn < te_ref - tol: break
        data = (data << 1) | (1 if ha > la else 0); bits += 1; i += 2
    if bits != 24: return None
    return ("Princeton", data & 0xFFFFFF, 24)

def decode_pwm(d, sref, sdelta, ts, tl, dsh, dlo, inv):
    n = len(d)
    if n < 3 or d[0] <= 0 or not within(d[0], sref, sdelta): return None
    data = 0; bits = 0; i = 1
    while i + 1 < n:
        low, high = d[i], d[i+1]
        if low >= 0 or high <= 0: break
        la, ha = -low, high
        if within(la, tl, dlo) and within(ha, ts, dsh): bit = 0 if inv else 1
        elif within(la, ts, dsh) and within(ha, tl, dlo): bit = 1 if inv else 0
        else: break
        data = (data << 1) | bit; bits += 1; i += 2
    return (data, bits)

def decode(d, fm=False):
    d = repair_short_glitches(d)
    if fm:
        r = decode_pwm(d, 555, 120, 555, 1111, 120, 120, True)
        if r and r[1] == 12: return ("Ansonic", r[0], 12)
        return None
    r = decode_smc5326(d)                                    # 25-bit + stop high (before Princeton)
    if r: return r
    r = decode_princeton(d)                                  # princeton/EV1527
    if r: return r
    r = decode_linear(d)
    if r: return r
    r = decode_phf(d, 385, 2695, 150, 450, 4000, 18)         # clemsa
    if r: return ("Clemsa", r[0], 18)
    r = decode_dooya(d)                                      # dooya (preamble-gated)
    if r: return r
    r = decode_intertechno(d)                                # intertechno (sync-gated)
    if r: return r
    r = decode_pwm(d, 700, 300, 350, 700, 100, 300, False)   # gate_tx
    if r and r[1] == 24: return ("GateTX", r[0], 24)
    r = decode_pwm(d, 320, 150, 320, 640, 150, 150, False)   # came
    if r and r[1] in (12,18,24,25,42): return ("CAME", r[0], r[1])
    r = decode_pwm(d, 700, 200, 700, 1400, 200, 200, False)  # nice
    if r and 12 <= r[1] <= 24: return ("NiceFlo", r[0], r[1])
    r = decode_pwm(d, 800, 450, 400, 1100, 150, 450, False)  # doitrand
    if r and r[1] == 37: return ("Doitrand", r[0], 37)
    r = decode_pwm(d, 2562, 400, 427, 853, 100, 300, True)   # phoenix_v2
    if r and r[1] == 52: return ("Phoenix_V2", r[0], 52)
    r = decode_magellan(d)                                   # magellan (CRC-gated, last)
    if r: return r
    return None

# ---------- FULL on-air stream builders (Flipper rules, headers INCLUDED) ----------
def air_linear(key):                 # H,L per bit; last bit low = trailer gap
    out = []
    for i in range(9, 0, -1):
        out += [1500, -500] if (key >> i) & 1 else [500, -1500]
    out += [1500, -(500*42)] if (key & 1) else [500, -(500*44)]   # trailer gap
    return out

def air_gate(key):                   # header 350*49 low, start 700 high, L-H pairs
    out = [-(350*49), 700]
    for i in range(23, -1, -1):
        out += [-700, 350] if (key >> i) & 1 else [-350, 700]
    return out

def air_ansonic(key):                # header 555*35 low, start 555 high, inverted
    out = [-(555*35), 555]
    for i in range(11, -1, -1):
        out += [-555, 1111] if (key >> i) & 1 else [-1111, 555]
    return out

def air_came(key, bits):             # AM reference (unchanged protocol) for control
    out = [-(320*47), 320]
    for i in range(bits-1, -1, -1):
        out += [-640, 320] if (key >> i) & 1 else [-320, 640]
    return out

def air_doitrand(key):               # header 400*62=24800 low, start 700, L-H, 37 bits
    out = [-(400*62), 700]
    for i in range(36, -1, -1):
        out += [-1100, 400] if (key >> i) & 1 else [-400, 1100]
    return out

def air_phoenix(key):                # header 427*60=25620 low, start 2562, L-H inverted, 52 bits
    out = [-(427*60), 2562]
    for i in range(51, -1, -1):
        out += [-427, 853] if (key >> i) & 1 else [-853, 427]
    return out

def air_clemsa(key):                 # HIGH-first 7:1, 18 bits, trailer gap ~19-21 ms
    out = []
    for i in range(17, 0, -1):
        out += [2695, -385] if (key >> i) & 1 else [385, -2695]
    out += [2695, -(385 + 2695*7)] if (key & 1) else [385, -(2695*8)]
    return out

def air_ev1527(key, te=350):         # EV1527/PT2262: 24-bit 3:1 H-L, sync = Te + 31*Te low
    out = []
    for i in range(23, -1, -1):
        out += [te*3, -te] if (key >> i) & 1 else [te, -te*3]   # bit1 / bit0
    out += [te, -(te*31)]            # sync bit; the 31*Te gap ends the frame
    return out

def mag_key(payload):                # 24-bit payload + CRC8 (low byte)
    return (payload << 8) | magellan_crc8([(payload>>16)&0xFF, (payload>>8)&0xFF, payload&0xFF])

def air_magellan(key):               # header + 12 sync + marker + start + 32 bits + stop + 40 ms sep
    out = [800, -200]
    for _ in range(12): out += [200, -200]
    out += [200, -400, 1200, -400]
    for i in range(31, -1, -1): out += [200, -400] if (key >> i) & 1 else [400, -200]
    out += [200, -40000]             # stop high + inter-frame separator (ends reception)
    return out

def air_intertechno(key):            # header + 38*Te sep + sync (short + 10*Te) + 32 two-pair bits
    ts, tl = 275, 1375
    out = [ts, -(ts*38), ts, -(ts*10)]
    for i in range(31, -1, -1):
        out += [ts, -tl, ts, -ts] if (key >> i) & 1 else [ts, -ts, ts, -tl]
    return out

def jitter(seq, pct):
    return [int(round(abs(v)*(1+pct))) * (1 if v >= 0 else -1) for v in seq]

def pipeline(frame_builder, repeats, fm=False, pct=0.0, trailing_gap=True):
    """Build `repeats` frames back-to-back, run the RMT+normalizer pipeline, and
    return the list of decoded (proto,key,bits) for every burst >= MIN_EDGES."""
    stream = []
    for _ in range(repeats):
        stream += frame_builder
    if trailing_gap:
        stream += [-(500*44)]        # final inter-frame gap so the last frame ends
    stream = jitter(stream, pct) if pct else stream
    out = []
    for rec in rmt_segment(stream):          # hardware receptions
        for f in sw_split(normalize(rec)):   # software frames
            out.append(decode(f, fm=fm))
    return out

print("== P0: full on-air stream (headers 17-21 ms) segments + decodes per frame ==")
cases = [
    ("Linear 0x2AB", air_linear(0x2AB),  False, ("Linear", 0x2AB, 10)),
    ("Linear 0x3FF", air_linear(0x3FF),  False, ("Linear", 0x3FF, 10)),
    ("Linear 0x000", air_linear(0x000),  False, ("Linear", 0x000, 10)),
    ("GateTX 0xABCDEF", air_gate(0xABCDEF), False, ("GateTX", 0xABCDEF, 24)),
    ("GateTX 0x800001", air_gate(0x800001), False, ("GateTX", 0x800001, 24)),
    ("Ansonic 0xABC", air_ansonic(0xABC), True,  ("Ansonic", 0xABC, 12)),
    ("Ansonic 0x801", air_ansonic(0x801), True,  ("Ansonic", 0x801, 12)),
    ("Doitrand 0x123456789", air_doitrand(0x123456789), False, ("Doitrand", 0x123456789, 37)),
    ("Phoenix_V2 0xABCDEF012345", air_phoenix(0xABCDEF012345), False, ("Phoenix_V2", 0xABCDEF012345, 52)),
    ("Clemsa 0x2AB3C", air_clemsa(0x2AB3C), False, ("Clemsa", 0x2AB3C, 18)),
    # EV1527/PT2262 learning code decodes via the Princeton codec (same waveform).
    ("EV1527 0xABC123", air_ev1527(0xABC123), False, ("Princeton", 0xABC123, 24)),
    ("EV1527 0x5A5A5A", air_ev1527(0x5A5A5A), False, ("Princeton", 0x5A5A5A, 24)),
    ("EV1527 te=200 0x0F0F0F", air_ev1527(0x0F0F0F, te=200), False, ("Princeton", 0x0F0F0F, 24)),
    # Magellan: sync-preamble + CRC8, 40 ms inter-frame gap (own reception each).
    ("Magellan 0xABCDEF", air_magellan(mag_key(0xABCDEF)), False, ("Magellan", mag_key(0xABCDEF), 32)),
    ("Magellan 0x123456", air_magellan(mag_key(0x123456)), False, ("Magellan", mag_key(0x123456), 32)),
    # Intertechno: 5:1 two-pair bits, ~10.4 ms inter-frame gap (own reception each).
    ("Intertechno 0x12345678", air_intertechno(0x12345678), False, ("Intertechno_V3", 0x12345678, 32)),
    ("Intertechno 0x0F0F0F0F", air_intertechno(0x0F0F0F0F), False, ("Intertechno_V3", 0x0F0F0F0F, 32)),
]
for label, frame, fm, want in cases:
    for reps in (1, 3, 5):
        got = pipeline(frame, reps, fm=fm)
        decoded = [g for g in got if g is not None]
        ok = len(decoded) == reps and all(g == want for g in decoded)
        check(ok, f"{label} x{reps} -> {len(decoded)}/{reps} frames, all == {want}")

print("== P0: header LOW actually TERMINATES a burst (not merged into the frame) ==")
# A gate stream of 3 frames must segment into >=3 decodable bursts; if the 17 ms
# header did NOT exceed the 10 ms threshold, the frames would fuse into one burst
# and decode would fail / yield a single wrong result.
bursts = rmt_segment([v for _ in range(3) for v in air_gate(0x0F0F0F)] + [-(500*44)])
decodable = [decode(normalize(b)) for b in bursts if len(normalize(b)) >= MIN_EDGES]
check(len(decodable) == 3 and all(x == ("GateTX", 0x0F0F0F, 24) for x in decodable),
      f"3x GateTX fused? -> {len(decodable)} separate decodable frames")
# And the longest intra-frame low (gate = 700 us) must NOT split a frame:
one = rmt_segment(air_gate(0x0F0F0F) + [-(500*44)])
one_dec = [decode(normalize(b)) for b in one if len(normalize(b)) >= MIN_EDGES]
check(one_dec == [("GateTX", 0x0F0F0F, 24)], f"single frame stays whole -> {one_dec}")

print("== P0: pipeline holds under jitter within tolerance ==")
for pct in (0.05, 0.10, -0.05, -0.10):
    got = [g for g in pipeline(air_gate(0xABCDEF), 3, pct=pct) if g]
    check(got == [("GateTX", 0xABCDEF, 24)]*3, f"GateTX pipeline jitter {pct:+.0%} -> {len(got)} ok")
for pct in (0.05, -0.05):
    got = [g for g in pipeline(air_ansonic(0xABC), 3, fm=True, pct=pct) if g]
    check(got == [("Ansonic", 0xABC, 12)]*3, f"Ansonic pipeline jitter {pct:+.0%} -> {len(got)} ok")

print("== P0 control: a known-good AM protocol (CAME) survives the same pipeline ==")
got = [g for g in pipeline(air_came(0xABCDEF, 24), 4) if g]
check(got == [("CAME", 0xABCDEF, 24)]*4, f"CAME-24 x4 through pipeline -> {len(got)} ok")

# ===================== Stage 7 Part C: capture boundary =====================
print("== Part C: software splits MANY frames in ONE hardware reception (short gap) ==")
# GateTX bodies (header dropped) separated by an 8 ms gap: 6 ms < 8 ms < 10 ms, so
# the RMT does NOT segment in hardware (single reception) and the software layer
# must split it. Terminated by a >10 ms gap (end of button press).
def gate_body(key): return air_gate(key)[1:]          # drop the 17 ms header low
def short_gap_stream(bodies, gap):
    s = []
    for b in bodies: s += b + [-gap]
    s += [-22000]                                     # final long gap ends reception
    return s
recs = rmt_segment(short_gap_stream([gate_body(0xABCDEF)]*5, 8000))
check(len(recs) == 1, f"5 short-gap frames stay in ONE hardware reception -> {len(recs)} rec(s)")
frames = sw_split(normalize(recs[0]))
check(len(frames) == 5, f"software splits that reception into 5 frames -> {len(frames)}")
dec = [decode(f) for f in frames]
check(dec == [("GateTX", 0xABCDEF, 24)]*5, f"all 5 decode identically (no loss/dup) -> {len(dec)}")
# distinct keys: proves no cross-frame contamination / duplication
keys = [0x111111, 0x222222, 0x333333, 0x444444]
recs = rmt_segment(short_gap_stream([gate_body(k) for k in keys], 7000))
dec = [decode(f) for f in sw_split(normalize(recs[0]))]
check(dec == [("GateTX", k, 24) for k in keys], f"4 distinct keys recovered in order -> {dec != None}")

print("== Part C: a 9-10 ms gap splits, a long VALID pulse (2.7 ms) does not ==")
# A frame gap in the 6-10 ms band is a boundary; Clemsa's 2695 us long pulse is
# data. Use >= MIN_EDGES-long segments so the split result is not filtered away.
seg = [700, -350, 350, -350, 700, -350, 350, -350, 700]          # 9 marks/spaces
check(len(sw_split(seg + [-9000] + seg)) == 2, "9 ms low splits into 2 frames")
check(len(sw_split(seg + [-7000] + seg)) == 2, "7 ms low splits into 2 frames")
longpulse = [2695, -385, 2695, -385, 2695, -385, 2695, -385, 2695]  # 2.695 ms pulses
check(len(sw_split(longpulse)) == 1, "2.695 ms valid pulses do NOT split (one frame)")
check(len(sw_split(seg + [-5999] + seg)) == 1, "a 5.999 ms low is below the frame gap -> no split")

print("== Part C: a long header ends the PREVIOUS reception, frame data stays intact ==")
# Two Doitrand frames (24.8 ms header each): the header segments in hardware and
# the 400/1100 us data pulses are never truncated.
got = [g for g in pipeline(air_doitrand(0x1F2E3D4C5), 3) if g]
check(got == [("Doitrand", 0x1F2E3D4C5, 37)]*3, f"long-header frames intact x3 -> {len(got)}")

print("== Part C: a PARTIAL frame is not reported as a valid decode ==")
partial = gate_body(0xABCDEF)[:1 + 2*15]              # start + only 15 of 24 bit pairs
fr = sw_split(normalize(rmt_segment(partial + [-22000])[0]))
check(all(decode(f) is None for f in fr), f"truncated 15/24-bit GateTX -> {[decode(f) for f in fr]}")
# a frame cut by buffer-full (no closing gap) is delivered but still not a valid decode
check(decode(normalize(partial)) is None, "partial frame without closing gap decodes to None")

print("== Part C: SMC5326 + Dooya short-gap SEGMENTATION + end-to-end decode ==")
def dooya_body(key):         # start H4758 L1466 + 40 full HIGH-first 2:1 pairs (MSB-first)
    out = [4758, -1466]      # the 40th bit's low merges into the next ~9 ms header
    for i in range(39, -1, -1):
        out += [733, -366] if (key >> i) & 1 else [366, -733]
    return out
def smc_body(key):           # 25 HIGH-first 3:1 pairs (MSB-first) + stop high
    out = []
    for i in range(24, -1, -1):
        out += [900, -300] if (key >> i) & 1 else [300, -900]
    out += [300]             # stop high; the te_short*25 guard low is the inter-frame gap
    return out
# Dooya end-to-end: 4 repeats, 9.2 ms gap between frames. The 40th bit's low merges
# with the header into the >6 ms separator, so each delivered frame ends on bit 40's
# lone high and decodes with the key (proves the frame is not split and bit 40 reads).
dkey = 0xE1DC030533
recs = rmt_segment(short_gap_stream([dooya_body(dkey)]*4, 9200))
frames = sw_split(normalize(recs[0]))
check(len(recs) == 1 and len(frames) == 4,
      f"Dooya 9.2 ms gap: 4 frames from 1 reception -> {len(frames)}")
check([decode(f) for f in frames] == [("Dooya", dkey, 40)]*4,
      f"Dooya each segmented frame decodes (bit 40 lone-high read) -> {[decode(f) for f in frames]}")
dkeys = [0x0000000001, 0x8000000000, 0x5AA55AA55A, 0x123456789A]
decd = [decode(f) for f in sw_split(normalize(rmt_segment(short_gap_stream([dooya_body(k) for k in dkeys], 9200))[0]))]
check(decd == [("Dooya", k, 40) for k in dkeys], f"Dooya distinct keys in order -> {decd}")
# SMC5326 end-to-end: 4 repeats of one key, 7.2 ms guard between words -> 4 frames,
# each decodes to the same key (guard stripped, stop high kept, starts on a mark).
recs = rmt_segment(short_gap_stream([smc_body(0x15AB3C5)]*4, 7200))
frames = sw_split(normalize(recs[0]))
check(len(recs) == 1 and len(frames) == 4,
      f"SMC5326 7.2 ms gap: 4 frames from 1 reception -> {len(frames)}")
check([decode(f) for f in frames] == [("SMC5326", 0x15AB3C5, 25)]*4,
      f"SMC5326 each segmented frame decodes -> {[decode(f) for f in frames]}")
# distinct keys recovered in order (no cross-frame contamination / dup)
dkeys = [0x0000001, 0x1000000, 0x1555555, 0x0AAAAAA]
dec = [decode(f) for f in sw_split(normalize(rmt_segment(short_gap_stream([smc_body(k) for k in dkeys], 7200))[0]))]
check(dec == [("SMC5326", k, 25) for k in dkeys], f"SMC5326 distinct keys in order -> {dec}")

print("== Part C: regression — the 10 current protocols still 1 frame per reception ==")
# (Each uses a long header/trailer >10 ms, so the hardware segments them and the
#  software split is a no-op — proven by the per-frame counts above and here.)
for label, frame, fm, want in cases:
    got = [g for g in pipeline(frame, 2, fm=fm) if g]
    check(got == [want, want], f"regression {label} x2 -> {len(got)} frames")

print("== RX noise filters: RMT glitch boundary, RSSI hysteresis, RSSI median ==")
check(c_define(CAPTURE_SOURCE, "SUBGHZ_RX_MIN_NS") == RX_MIN_US * 1000,
      "RMT glitch setting matches pipeline's 3 us C5 model")
check(c_define(PROTOCOL_SOURCE, "SUBGHZ_DECODE_GLITCH_US") == DECODE_GLITCH_US,
      "decoder-only software glitch repair uses the tested 50 us threshold")
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_DEFAULT_RSSI_THR") == -90,
      "firmware uses Flipper Unleashed's -90 dBm RAW default")
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_RSSI_CAPTURE_HOLD_MS") == 400,
      "RSSI qualification is held across the RMT idle-stop callback")
check("SUBGHZ_READ_CONFIRM_REPEATS" not in SUBGHZ_SOURCE and
      "read_confirm_repeat" not in SUBGHZ_SOURCE and
      "read_candidate_confirmed" not in SUBGHZ_SOURCE,
      "Read does not require a second decoded transmission (reports first decode)")
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_READ_DEDUPE_MS") == 1500,
      "Read suppresses duplicate reports for 1.5 seconds")
check("if (truncated) {" in SUBGHZ_SOURCE and
      "raw_candidate_confirmed" not in SUBGHZ_SOURCE and
      c_define(SUBGHZ_SOURCE, "SUBGHZ_RAW_STRICT_DEDUPE_MS") == 1500,
      "truncated frames dropped up front; strict RAW only deduplicates after")
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_RAW_DEDUPE_TIMING_TOLERANCE_PCT") == 30 and
      c_define(SUBGHZ_SOURCE, "SUBGHZ_RAW_DEDUPE_MIN_TOLERANCE_US") == 50,
      "strict RAW duplicate suppression uses bounded pulse timing tolerance")
check("if (!read_mode && squelch_on && !rssi_squelch_is_open())" in SUBGHZ_SOURCE,
      "RSSI gate cannot discard protocol-validated Read frames")
check("age <= hold" in SUBGHZ_SOURCE and
      "SUBGHZ_RSSI_CAPTURE_HOLD_MS" in SUBGHZ_SOURCE,
      "RSSI hold accounts for delayed frame delivery after RMT stop")
check("if (truncated) {" in SUBGHZ_SOURCE and
      re.search(r"decoded\s*=\s*subghz_protocol_decode", SUBGHZ_SOURCE) is not None,
      "truncated frames return before decode, so they cannot become Read results")
check("READ proto=unknown" not in SUBGHZ_SOURCE and
      "report_unknown" not in SUBGHZ_SOURCE and
      "read_unknown_should_report" not in SUBGHZ_SOURCE,
      "READ reports ONLY decoded protocols; undecoded noise is never listed")
check("memcpy(out, durations" in PROTOCOL_SOURCE and
      "s_decode_durations" in PROTOCOL_SOURCE,
      "software glitch repair works on a private buffer, preserving RAW data")
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_RSSI_SAMPLE_COUNT") == 3,
      "firmware uses three RSSI samples per sweep point")
check(rmt_segment([100, -2]) == [[100]],
      "2 us glitch removed")
check(rmt_segment([100, -3]) == [[100, -3]],
      "3 us pulse retained")
check(repair_short_glitches([500, -12, 600]) == [1112],
      "isolated short glitch is absorbed and total elapsed time is preserved")
check(repair_short_glitches([500, -49, 500]) == [1049] and
      repair_short_glitches([500, -50, 500]) == [500, -50, 500],
      "software glitch threshold removes 49 us but retains 50 us")
check(repair_short_glitches([320, -320, 320]) == [320, -320, 320],
      "valid 320 us protocol timing is retained")
# Capture-layer de-speckle (symbols_to_durations / SUBGHZ_RX_DESPECKLE_US): the
# RAW stream itself is cleaned, not just a decoder-private copy, matching Flipper.
check(c_define(CAPTURE_SOURCE, "SUBGHZ_RX_DESPECKLE_US") == 50,
      "capture de-speckle uses Flipper's 50 us te_short threshold")
check(normalize([500, -12, 600]) == [1112],
      "a 12 us opposite-level spike is absorbed into the surrounding level")
check(normalize([500, -60, 500]) == [500, -60, 500],
      "a 60 us pulse (>= 50) is kept as a real edge")
check(normalize([400, -10, 20, -15, 500]) == [945],
      "a run of sub-50 us noise collapses to a single level in the RAW stream")
check(normalize([30, 500, -400, 600]) == [500, -400, 600],
      "a leading sub-50 us runt mark is dropped")
check(normalize([400, -320, 320]) == [400, -320, 320],
      "valid 320 us protocol pulses survive de-speckle unchanged")
gate_noisy = gate_body(0xABCDEF)
gate_noisy[1:2] = [-350, 12, -338]
check(12 in gate_noisy and decode(gate_noisy) == ("GateTX", 0xABCDEF, 24),
      "decoder repairs an interior glitch without mutating the RAW capture")
gate_capture = [("GateTX", 0xABCDEF, 24), ("GateTX", 0xABCDEF, 24)]
check(read_reports([(gate_capture[0], 0)]) == [True],
      "one complete known-protocol frame is reported immediately")
check(read_reports([(("GateTX", 0xABCDEF, 24, 700), 0),
                    (("GateTX", 0xABCDEF, 24, 740), 100)]) == [True, False],
      "Read deduplicates repeats without delaying the first decode")
check(read_reports([(gate_capture[0], 0), (None, 50),
                    (gate_capture[0], 100)]) == [True, False, False],
      "an undecoded burst does not delay or erase a valid one-frame decode")
check(read_reports([(gate_capture[0], 0), (gate_capture[0], 1501)]) ==
      [True, True],
      "Read reports the same key again after the dedupe interval")
check(read_reports([(None, 0), (None, 100), (None, 5000)]) == [False, False, False],
      "undecoded noise bursts are never reported (READ lists only decoded)")
check(read_reports([(gate_capture[0], 0),
                    (("GateTX", 0xABCDEE, 24), 100)]) == [True, True],
      "a different decoded key is reported immediately")
raw_frame = [400, -400, 800, -400, 400, -800]
raw_jitter = [490, -350, 980, -480, 330, -1020]
check(raw_timings_match(raw_frame, raw_jitter),
      "strict RAW matches same pulse shape with small timing jitter")
check(not raw_timings_match(raw_frame, [400, -400, 800, -400, 400]),
      "strict RAW rejects a different pulse count")
check(not raw_timings_match(raw_frame, [400, -400, -800, 400, 400, -800]),
      "strict RAW rejects changed pulse polarity")
check(strict_raw_accepts(raw_frame, False),
      "strict RAW accepts its first complete one-shot capture")
check(not strict_raw_accepts(raw_frame, True),
      "strict RAW rejects a truncated capture without awaiting another frame")
# Continuous Read RAW recorder (default mode): all signals in ONE recording.
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_RAWREC_GAP_US") == RAWREC_GAP_US,
      "recorder inserts a 15 ms synthesized inter-frame gap")
_A, _B = [400, -400, 800], [500, -500, 900]
check(rawrec_accumulate([_A])[0] == _A,
      "the first recorded signal is appended with no leading gap")
check(rawrec_accumulate([_A, _B])[0] == [400, -400, 800, -15000, 500, -500, 900],
      "a second signal is appended after one inter-frame gap (all in one recording)")
_rec3, _trunc3 = rawrec_accumulate([_A, _B, _A])
check(strictly_alternating(_rec3) and not _trunc3,
      "the growing recording stays strictly sign-alternating and fits the buffer")
_big = [[1] * 200 for _ in range(10)]          # 10 x 200 + 9 gaps = 2009 > 1024
_recb, _truncb = rawrec_accumulate(_big)
check(len(_recb) <= RAW_MAX and _truncb,
      "recorder caps at 1024 timings and flags truncation when the buffer fills")
check(rawrec_accumulate([_A, [1] * (RAW_MAX - 1)])[0] == _A,
      "a frame that would overflow the recording is dropped whole, not split")
check(rssi_recently_qualified(True, 1000, 1400),
      "RSSI above threshold remains qualified through a 400 ms RMT callback delay")
check(not rssi_recently_qualified(True, 1000, 1401),
      "stale RSSI qualification expires instead of opening RAW indefinitely")
check(not rssi_recently_qualified(False, 0, 50),
      "no above-threshold sample does not qualify default RAW mode")
check(rssi_recently_qualified(False, 0, 50, threshold=-128),
      "explicitly disabled RSSI threshold accepts RAW captures")
check(median3(-100, -70, -101) == -100,
      "median-of-three rejects one isolated high RSSI sample")
# Analyzer: user-adjustable trigger (Flipper-faithful), not a fixed threshold.
_ANALYZER_TRIGGER = c_define(SUBGHZ_SOURCE, "SUBGHZ_ANALYZER_DEFAULT_TRIGGER")
check(_ANALYZER_TRIGGER is not None and -110 <= _ANALYZER_TRIGGER <= 0,
      f"analyzer default trigger is a sane dBm value ({_ANALYZER_TRIGGER})")
check("SUBGHZ_ANALYZER_MIN_RSSI" not in SUBGHZ_SOURCE,
      "the fixed absolute -93 dBm threshold is gone (replaced by the trigger)")
check("SUBGHZ_ANALYZER_MIN_MARGIN_DB" not in SUBGHZ_SOURCE and
      "SUBGHZ_ANALYZER_MIN_FLOOR_SAMPLES" not in SUBGHZ_SOURCE,
      "the buggy relative noise-floor margin gate is removed")
check(c_define(SUBGHZ_SOURCE, "SUBGHZ_ANALYZER_REFINE_RADIUS_HZ") == 300000 and
      c_define(SUBGHZ_SOURCE, "SUBGHZ_ANALYZER_REFINE_STEP_HZ") == 20000,
      "fine scan uses Flipper's +/-300 kHz / 20 kHz grid")
check("SUBGHZ_ANALYZER_BW_COARSE 0x17" in SUBGHZ_SOURCE and
      "SUBGHZ_ANALYZER_BW_FINE   0xF7" in SUBGHZ_SOURCE,
      "coarse hop uses 650 kHz BW, fine scan uses narrow 58 kHz BW (noise reject)")
check("SUBGHZ_ANALYZER_NOISE_NEAR_HZ" not in SUBGHZ_SOURCE and
      "SUBGHZ_ANALYZER_NOISE_FAR_HZ" not in SUBGHZ_SOURCE,
      "the slow extra sideband scans are gone (gate adds no scan time)")
check("SUBGHZ_ANALYZER_AGCCTRL1 0x08" in SUBGHZ_SOURCE and
      "SUBGHZ_ANALYZER_AGCCTRL0 0x30" in SUBGHZ_SOURCE and
      "SUBGHZ_ANALYZER_MDMCFG3  0x7F" in SUBGHZ_SOURCE,
      "analyzer applies Flipper's analyzer-specific AGC/modem registers")
# Detection requires BOTH coarse and fine peaks above the trigger.
check(analyzer_accept(-60, -62),
      "a strong signal (both stages above the default trigger) is detected")
check(not analyzer_accept(-95, -96),
      "noise near the floor (both stages below the trigger) is NOT detected")
check(not analyzer_accept(-60, -95),
      "a coarse hit the narrow fine scan cannot confirm is rejected")
check(not analyzer_accept(-95, -60),
      "a fine hit without a coarse hit above the trigger is rejected")
check(analyzer_accept(-85, -85, trigger=-90) and
      not analyzer_accept(-85, -85, trigger=-80),
      "raising the trigger above a weak peak stops it being reported (user-tunable)")
# Auto-calibration: trigger is set a margin above the MEASURED band floor so
# noise is rejected regardless of the hardware floor (the phantom-detection fix).
_ACM = c_define(SUBGHZ_SOURCE, "SUBGHZ_ANALYZER_AUTOCAL_MARGIN_DB")
check(_ACM is not None and 5 <= _ACM <= 25,
      f"analyzer auto-cal margin above the noise floor is sane ({_ACM} dB)")
check("analyzer_autocal" in SUBGHZ_SOURCE and "ANALYZER_TRIG" in SUBGHZ_SOURCE,
      "analyzer auto-calibrates the trigger to the measured floor at start")
# With auto-cal the trigger sits margin dB above the floor: band noise (= floor)
# is below it, a real carrier (>> floor) is above it.
_floor = -95
_auto_trigger = _floor + _ACM
check(not analyzer_accept(_floor, _floor, trigger=_auto_trigger),
      "after auto-cal, band noise at the floor is NOT reported")
check(analyzer_accept(_floor + 30, _floor + 30, trigger=_auto_trigger),
      "after auto-cal, a carrier well above the floor IS reported")

# EV1527 / PT2262 support: the common 433/315 learning code is the Princeton
# waveform; aliases resolve to the Princeton codec for emulate/.sub interop.
check('"EV1527"' in PROTOCOL_SOURCE and '"PT2262"' in PROTOCOL_SOURCE,
      "EV1527/PT2262 accepted as names (alias to the Princeton codec)")
# Holtek HT12X (12-bit) is byte-identical on-air to CAME-12 (no distinguishing
# header/checksum; the only difference, preamble length, is stripped), so there is
# no separate decoder — the names alias to the CAME codec for emulate/.sub interop.
check('"HT12X"' in PROTOCOL_SOURCE and '"Holtek_HT12X"' in PROTOCOL_SOURCE,
      "HT12X/Holtek_HT12X accepted as names (alias to the CAME codec)")
print()
print("RESULT:", "ALL PASS" if FAIL == 0 else f"{FAIL} FAILURES")
import sys; sys.exit(1 if FAIL else 0)
