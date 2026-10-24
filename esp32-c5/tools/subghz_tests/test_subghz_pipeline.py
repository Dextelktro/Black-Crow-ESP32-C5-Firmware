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

FAIL = 0
def check(cond, msg):
    global FAIL
    print(f"  {'PASS' if cond else 'FAIL'} {msg}")
    if not cond: FAIL += 1

# ---------- capture pipeline mirror (subghz_capture.c) ----------
RX_MIN_US    = 50       # SUBGHZ_RX_MIN_NS   (glitch filter)
RX_MAX_US    = 10000    # SUBGHZ_RX_MAX_NS   (HW: a level > this ends a reception)
FRAME_GAP_US = 6000     # SUBGHZ_FRAME_GAP_US (SW: a low >= this ends a frame)
MIN_EDGES    = 8        # SUBGHZ_RX_MIN_EDGES

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

def normalize(rec):
    """Mirror of symbols_to_durations(): merge consecutive same-sign, drop the
    leading space(s) and the final trailing space, no zeros. Interior gaps are
    KEPT (the software splitter needs them)."""
    out = []
    for v in rec:
        if v == 0:
            continue
        if not out:
            if v < 0:
                continue                # drop leading space
            out.append(v)
        elif (out[-1] < 0) == (v < 0):
            out[-1] += v                # same level: merge
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
    if fm:
        r = decode_pwm(d, 555, 120, 555, 1111, 120, 120, True)
        if r and r[1] == 12: return ("Ansonic", r[0], 12)
        return None
    r = decode_linear(d)
    if r: return r
    r = decode_phf(d, 385, 2695, 150, 450, 4000, 18)         # clemsa
    if r: return ("Clemsa", r[0], 18)
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

print("== Part C: short-gap SEGMENTATION for Dooya / SMC5326 (no decoder yet) ==")
# Faithful-ish inter-frame gaps; we only assert the splitter separates frames.
def dooya_body(bits=40):     # start H4758 L1466 + 40 HIGH-first pairs (fixed pattern)
    out = [4758, -1466]
    for i in range(bits): out += [733, -366] if (i & 1) else [366, -733]
    return out
def smc_body(bits=25):       # 25 HIGH-first 3:1 pairs te 300/900 (fixed pattern)
    out = []
    for i in range(bits): out += [900, -300] if (i & 1) else [300, -900]
    out += [300]             # stop high
    return out
recs = rmt_segment(short_gap_stream([dooya_body()]*4, 9200))   # Dooya ~9.2 ms gap
check(len(recs) == 1 and len(sw_split(normalize(recs[0]))) == 4,
      f"Dooya 9.2 ms gap: 4 frames from 1 reception -> {len(sw_split(normalize(recs[0])))}")
recs = rmt_segment(short_gap_stream([smc_body()]*4, 7200))     # SMC5326 ~7.2 ms gap
check(len(recs) == 1 and len(sw_split(normalize(recs[0]))) == 4,
      f"SMC5326 7.2 ms gap: 4 frames from 1 reception -> {len(sw_split(normalize(recs[0])))}")

print("== Part C: regression — the 10 current protocols still 1 frame per reception ==")
# (Each uses a long header/trailer >10 ms, so the hardware segments them and the
#  software split is a no-op — proven by the per-frame counts above and here.)
for label, frame, fm, want in cases:
    got = [g for g in pipeline(frame, 2, fm=fm) if g]
    check(got == [want, want], f"regression {label} x2 -> {len(got)} frames")

print()
print("RESULT:", "ALL PASS" if FAIL == 0 else f"{FAIL} FAILURES")
import sys; sys.exit(1 if FAIL else 0)
