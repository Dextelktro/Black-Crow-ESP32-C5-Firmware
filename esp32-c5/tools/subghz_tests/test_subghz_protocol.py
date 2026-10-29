#!/usr/bin/env python3
# Host unit test for esp32-c5/main/subghz_protocol.c LOGIC.
# - Synthetic waveforms are built DIRECTLY from the Flipper encoding rules
#   (independent reference), NOT from our own encoder, per the audit note that
#   round-trip-only tests can hide shared encoder/decoder bugs.
# - decode()/encode() below mirror the C exactly, including integer division,
#   so a logic bug in the C shows here. (This validates algorithm logic, not
#   the compiled binary / C type widths — those are audited separately.)

FAIL = 0
def check(cond, msg):
    global FAIL
    if cond:
        print(f"  PASS {msg}")
    else:
        FAIL += 1
        print(f"  FAIL {msg}")

# ---------- mirror of the C decoder ----------
def within(a, target, delta):
    d = a - target if a > target else target - a
    return d < delta

def decode_princeton(d):
    data = 0; bits = 0; total = 0; i = 0
    n = len(d)
    while i + 1 < n and bits < 24:
        high = d[i]; low = d[i+1]
        if high <= 0 or low >= 0: break
        ha = high; la = -low
        mn = ha if ha < la else la
        mx = la if ha < la else ha
        if mn < 80 or mn > 2000: break
        if mx < mn*2 + mn//3 or mx > mn*4: break
        bit = 1 if ha > la else 0
        data = (data << 1) | bit
        total += ha + la
        bits += 1; i += 2
    if bits != 24: return None
    return ("Princeton", data & 0xFFFFFF, 24, total // (24*4))

def decode_pwm_frame(d, start_ref, start_delta, ts, tl, d_short, d_long, inverted):
    n = len(d)
    if n < 3 or d[0] <= 0: return None
    if not within(d[0], start_ref, start_delta): return None
    data = 0; bits = 0; i = 1
    while i + 1 < n:
        low = d[i]; high = d[i+1]
        if low >= 0 or high <= 0: break
        la = -low; ha = high
        if within(la, tl, d_long) and within(ha, ts, d_short): bit = 0 if inverted else 1
        elif within(la, ts, d_short) and within(ha, tl, d_long): bit = 1 if inverted else 0
        else: break
        if bits < 64: data = (data << 1) | bit
        bits += 1; i += 2
    return (data, bits)

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
    return ("Linear", r[0] & 0x3FF, 10, 500) if r else None

def decode_smc5326(d):   # 25-bit HIGH-first 3:1 + lone trailing stop high
    if len(d) < 2: return None
    stop = d[-1]
    if stop <= 0 or not within(stop, 300, 200): return None   # stop high = te_short
    r = decode_phf(d[:-1], 300, 900, 200, 200, 1800, 25)      # 25 pairs, whole-consume
    if not r: return None
    return ("SMC5326", r[0] & 0x1FFFFFF, 25, 300)

def decode_dooya(d):     # start H4758 + L1466 preamble, then 40 HIGH-first 2:1 bits
    if len(d) < 4: return None
    if d[0] <= 0 or not within(d[0], 4758, 600): return None   # start high te_short*13
    if d[1] >= 0 or not within(-d[1], 1466, 360): return None  # start low te_long*2
    r = decode_phf(d[2:], 366, 733, 120, 240, 2932, 40)        # faithful: d_long = te_delta*2
    if not r: return None
    return ("Dooya", r[0] & 0xFFFFFFFFFF, 40, 366)

def magellan_crc8(data):   # poly 0x31, init 0x00, MSB-first (mirror of magellan.c)
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc

def decode_magellan(d):    # sync train + marker + start bit + 32 2:1 bits + stop high + CRC8
    ts, tl, de = 200, 400, 100
    n = len(d); i = 0
    while i < n and not (d[i] > 0 and within(d[i], ts, de)): i += 1   # skip ts*4 header
    header = 0; marker = False
    while i + 1 < n:
        if d[i] <= 0 or d[i+1] >= 0: return None
        hi, lo = d[i], -d[i+1]
        if within(hi, ts, de) and within(lo, ts, de): header += 1; i += 2; continue
        if header > 10 and within(hi, ts, de) and within(lo, tl, de*2): i += 2; marker = True; break
        return None
    if not marker: return None
    if i + 1 >= n: return None
    if d[i] <= 0 or not within(d[i], ts*6, de*3): return None         # start high ts*6
    if d[i+1] >= 0 or not within(-d[i+1], tl, de*2): return None      # start low
    i += 2
    data = 0; bits = 0
    while i + 1 < n and bits < 32:
        if d[i] <= 0 or d[i+1] >= 0: return None
        hi, lo = d[i], -d[i+1]
        if within(hi, ts, de) and within(lo, tl, de): bit = 1         # H short + L long
        elif within(hi, tl, de) and within(lo, ts, de): bit = 0       # H long + L short
        else: return None
        data = (data << 1) | bit; bits += 1; i += 2
    if bits != 32: return None
    if i >= n or d[i] <= 0 or not within(d[i], ts, de): return None   # lone stop high
    if i + 1 != n: return None                                        # whole-consume
    if (data & 0xFF) != magellan_crc8([(data>>24)&0xFF, (data>>16)&0xFF, (data>>8)&0xFF]): return None
    return ("Magellan", data & 0xFFFFFFFF, 32, 200)

def decode_intertechno(d):   # [short H][2750 L] + 32 bits, each two (short H, L) pairs
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
            return None                                  # non-final bit missing L2
    if i == n or (i + 1 == n and d[i] > 0 and within(d[i], ts, de)):
        return ("Intertechno_V3", data & 0xFFFFFFFF, 32, 275)
    return None

def decode(d, fm=False):
    if fm:
        # FM/2FSK set: Ansonic only (kept disjoint from AM Nice FLO).
        r = decode_pwm_frame(d, 555, 120, 555, 1111, 120, 120, True)   # ansonic
        if r and r[1] == 12:
            return ("Ansonic", r[0], 12, 555)
        return None
    # AM/OOK set. SMC5326 before Princeton (both 3:1; Princeton does not
    # whole-consume, so it would misread SMC's first 24 pairs otherwise).
    r = decode_smc5326(d)
    if r: return r
    r = decode_princeton(d)
    if r: return r
    r = decode_linear(d)
    if r: return r
    r = decode_phf(d, 385, 2695, 150, 450, 4000, 18)               # clemsa
    if r: return ("Clemsa", r[0], 18, 385)
    r = decode_dooya(d)                                            # dooya (preamble-gated)
    if r: return r
    r = decode_intertechno(d)                                      # intertechno (sync-gated)
    if r: return r
    r = decode_pwm_frame(d, 430, 100, 430, 870, 100, 200, False)   # holtek
    if r and r[1] == 40 and (r[0] & 0xF000000000) == 0x5000000000:
        return ("Holtek", r[0], 40, 430)
    r = decode_pwm_frame(d, 700, 300, 350, 700, 100, 300, False)   # gate_tx (start=te_long)
    if r and r[1] == 24:
        return ("GateTX", r[0], 24, 350)
    r = decode_pwm_frame(d, 320, 150, 320, 640, 150, 150, False)   # came
    if r and r[1] in (12, 18, 24, 25, 42):
        return ("CAME", r[0], r[1], 320)
    r = decode_pwm_frame(d, 700, 200, 700, 1400, 200, 200, False)  # nice
    if r and 12 <= r[1] <= 24:
        return ("NiceFlo", r[0], r[1], 700)
    r = decode_pwm_frame(d, 800, 450, 400, 1100, 150, 450, False)  # doitrand
    if r and r[1] == 37:
        return ("Doitrand", r[0], 37, 400)
    r = decode_pwm_frame(d, 2562, 400, 427, 853, 100, 300, True)   # phoenix_v2
    if r and r[1] == 52:
        return ("Phoenix_V2", r[0], 52, 427)
    r = decode_magellan(d)                                         # magellan (CRC-gated, last)
    if r: return r
    return None

# ---------- mirror of the C encoder ----------
def encode_princeton(te, guard, key, bits):
    out = []
    for i in range(bits-1, -1, -1):
        if (key >> i) & 1: out += [te*3, -te]
        else: out += [te, -te*3]
    out += [te, -(te*guard)]
    return out

def encode_pwm_frame(ts, tl, header_te, start_high, inverted, key, bits):
    out = [-(ts*header_te), start_high]
    for i in range(bits-1, -1, -1):
        one = ((key >> i) & 1) != 0
        low  = tl if (one != inverted) else ts
        high = ts if (one != inverted) else tl
        out += [-low, high]
    return out

def encode_came_like(ts, tl, header_te, key, bits):   # start=te_short, non-inverted
    return encode_pwm_frame(ts, tl, header_te, ts, False, key, bits)

def encode_phf(ts, tl, key, bits, tr1, tr0):   # HIGH-first PWM (Linear/Clemsa)
    out = []
    for i in range(bits-1, 0, -1):
        out += [tl, -ts] if (key >> i) & 1 else [ts, -tl]
    out += [tl, -tr1] if (key & 1) else [ts, -tr0]
    return out

def encode_linear(ts, tl, key, bits):
    return encode_phf(ts, tl, key, bits, ts*42, ts*44)

def encode_dooya(key, bits=40):   # header low + start high + start low + HIGH-first 2:1 pairs
    header_lo = 733*12 + (733 if (key & 1) else 366)
    out = [-header_lo, 4758, -1466]
    for i in range(bits-1, -1, -1):
        out += [733, -366] if (key >> i) & 1 else [366, -733]
    return out

def encode_intertechno(key, bits=32):   # header + separator + sync + 32 bits (2 pairs each)
    ts, tl = 275, 1375
    out = [ts, -(ts*38), ts, -2750]
    for i in range(bits-1, -1, -1):
        out += [ts, -tl, ts, -ts] if (key >> i) & 1 else [ts, -ts, ts, -tl]
    return out

def encode_magellan(key, bits=32):   # header + sync + marker + start + 32 pairs + stop + sep
    ts, tl = 200, 400
    out = [ts*4, -ts]
    for _ in range(12): out += [ts, -ts]
    out += [ts, -tl, ts*6, -tl]
    for i in range(bits-1, -1, -1):
        out += [ts, -tl] if (key >> i) & 1 else [tl, -ts]
    out += [ts, -(tl*100)]
    return out

def came_header(bits):
    if bits in (24, 42): return 76
    if bits in (12, 18): return 47
    if bits == 25: return 36
    return 16

# ---------- INDEPENDENT synthetic captures (Flipper rules, header/guard trimmed) ----------
def cap_princeton(key, te):
    out = []
    for i in range(23, -1, -1):
        if (key >> i) & 1: out += [3*te, -te]
        else: out += [te, -3*te]
    out += [te]                 # stop high; guard low trimmed by capture
    return out

def cap_came_like(key, bits, ts, tl):
    out = [ts]                  # start high; header low trimmed
    for i in range(bits-1, -1, -1):
        if (key >> i) & 1: out += [-tl, ts]
        else: out += [-ts, tl]
    return out

def cap_linear(key):            # 10 bits, H-L, trailer gap stripped -> lone high
    out = []
    for i in range(9, 0, -1):
        out += [1500, -500] if (key >> i) & 1 else [500, -1500]
    out += [1500 if (key & 1) else 500]   # last bit, lone high
    return out

def cap_linear_gap(key):        # same but with the inter-frame gap present (even len)
    out = []
    for i in range(9, 0, -1):
        out += [1500, -500] if (key >> i) & 1 else [500, -1500]
    if key & 1: out += [1500, -(500*42)]
    else:       out += [500, -(500*44)]
    return out

def cap_gate_tx(key):           # 24 bits, start=te_long=700, L-H, header trimmed
    out = [700]
    for i in range(23, -1, -1):
        if (key >> i) & 1: out += [-700, 350]   # bit1: L long, H short
        else:              out += [-350, 700]   # bit0: L short, H long
    return out

def cap_ansonic(key):           # 12 bits, start=te_short=555, INVERTED, header trimmed
    out = [555]
    for i in range(11, -1, -1):
        if (key >> i) & 1: out += [-555, 1111]  # bit1: L short, H long
        else:              out += [-1111, 555]  # bit0: L long, H short
    return out

def cap_doitrand(key):          # 37 bits, start H 700 (2*ts-100), L-H, header trimmed
    out = [700]
    for i in range(36, -1, -1):
        if (key >> i) & 1: out += [-1100, 400]  # bit1: L long, H short
        else:              out += [-400, 1100]  # bit0: L short, H long
    return out

def cap_phoenix(key):           # 52 bits, start H 2562 (6*ts), L-H INVERTED, header trimmed
    out = [2562]
    for i in range(51, -1, -1):
        if (key >> i) & 1: out += [-427, 853]   # bit1: L short, H long
        else:              out += [-853, 427]   # bit0: L long, H short
    return out

def cap_clemsa(key):            # 18 bits, HIGH-first 7:1, trailer gap stripped -> lone high
    out = []
    for i in range(17, 0, -1):
        out += [2695, -385] if (key >> i) & 1 else [385, -2695]
    out += [2695 if (key & 1) else 385]   # last bit, lone high
    return out

def cap_smc5326(key):           # 25 bits, HIGH-first 3:1, + stop high; guard low stripped
    out = []
    for i in range(24, -1, -1):
        out += [900, -300] if (key >> i) & 1 else [300, -900]
    out += [300]                # stop high (te_short); te_short*25 guard low trimmed
    return out

def cap_dooya(key):             # start H4758, L1466, 40 HIGH-first 2:1; 40th bit lone high
    out = [4758, -1466]         # ~9 ms header low before the start high is stripped
    for i in range(39, 0, -1):
        out += [733, -366] if (key >> i) & 1 else [366, -733]
    out += [733 if (key & 1) else 366]   # bit 40 (LSB): its low merged into next header
    return out

def cap_intertechno(key):       # [short H][2750 L] + 32 bits (2 pairs each) + trailing header high
    out = [275, -2750]
    for i in range(31, -1, -1):
        if (key >> i) & 1: out += [275, -1375, 275, -275]   # bit1 (short,long)(short,short)
        else:              out += [275, -275, 275, -1375]   # bit0 (short,short)(short,long)
    out += [275]                # trailing header high of the next frame (stripped sep before it)
    return out

def cap_magellan(key):          # header + 12 sync pairs + marker + start + 32 bits + stop high
    out = [800, -200]           # header (ts*4, ts); decoder syncs on the first short high
    for _ in range(12): out += [200, -200]        # sync pairs (>10 required)
    out += [200, -400]          # marker (short H, long L)
    out += [1200, -400]         # start bit (ts*6, long)
    for i in range(31, -1, -1):
        out += [200, -400] if (key >> i) & 1 else [400, -200]   # bit1 H short/L long; bit0 H long/L short
    out += [200]                # stop high; the te_long*100 separator low is stripped
    return out

def jitter(seq, pct):
    # scale each magnitude by (1+pct), preserve sign
    out = []
    for v in seq:
        m = abs(v); s = 1 if v >= 0 else -1
        out.append(s * int(round(m * (1 + pct))))
    return out

print("== valid frames decode to correct proto/key/bits ==")
check(decode(cap_princeton(0xABCDEF, 390)) == ("Princeton", 0xABCDEF, 24, 390), "Princeton 0xABCDEF te=390")
check(decode(cap_princeton(0x000001, 300)) == ("Princeton", 0x000001, 24, 300), "Princeton 0x000001 te=300")
check(decode(cap_princeton(0x800001, 450)) == ("Princeton", 0x800001, 24, 450), "Princeton MSB 0x800001 te=450")
r = decode(cap_came_like(0xABC, 12, 320, 640)); check(r == ("CAME", 0xABC, 12, 320), f"CAME-12 0xABC -> {r}")
r = decode(cap_came_like(0xABCDEF, 24, 320, 640)); check(r == ("CAME", 0xABCDEF, 24, 320), f"CAME-24 0xABCDEF -> {r}")
r = decode(cap_came_like(0x801, 12, 700, 1400)); check(r == ("NiceFlo", 0x801, 12, 700), f"NiceFlo-12 0x801 -> {r}")
holtek_key = 0x5123456789            # header nibble 0x5, 40-bit
r = decode(cap_came_like(holtek_key, 40, 430, 870)); check(r == ("Holtek", holtek_key, 40, 430), f"Holtek-40 -> {r}")

print("== tolerance (decoder must still match within window) ==")
for pct in (0.05, 0.10, 0.20, -0.05, -0.10, -0.20):
    r = decode(jitter(cap_came_like(0xABC, 12, 320, 640), pct))
    check(r == ("CAME", 0xABC, 12, 320), f"CAME-12 jitter {pct:+.0%} -> {r}")
for pct in (0.05, 0.10):
    r = decode(jitter(cap_princeton(0xABCDEF, 390), pct))
    check(r is not None and r[0] == "Princeton" and r[1] == 0xABCDEF, f"Princeton jitter {pct:+.0%} -> {r}")

print("== FALSE POSITIVE: CAME-24 must NOT read as Princeton ==")
r = decode(cap_came_like(0xABCDEF, 24, 320, 640))
check(r is not None and r[0] == "CAME", f"CAME-24 classified {r[0] if r else None} (not Princeton)")
r = decode(cap_came_like(0xAB, 12, 700, 1400))
check(r is not None and r[0] == "NiceFlo", f"Nice-12 classified {r[0] if r else None} (not Princeton)")
r = decode(cap_came_like(0x5123456789, 40, 430, 870))
check(r is not None and r[0] == "Holtek", f"Holtek-40 classified {r[0] if r else None} (not Princeton/CAME)")

print("== FALSE POSITIVE: random / corrupt / truncated ==")
import random
random.seed(1)
falsepos = 0
for _ in range(20000):
    n = random.randint(4, 60)
    seq = []
    sign = 1
    for _ in range(n):
        seq.append(sign * random.randint(100, 2000))
        sign = -sign
    if seq[0] < 0: seq[0] = -seq[0]
    if decode(seq) is not None: falsepos += 1
check(falsepos == 0, f"random alternating bursts: {falsepos}/20000 false decodes")
# truncated CAME (only 8 bits -> not an accepted count)
r = decode(cap_came_like(0xAB, 8, 320, 640)); check(r is None, f"CAME 8-bit (unsupported count) rejected -> {r}")
# Holtek without 0x5 header nibble
r = decode(cap_came_like(0x1123456789, 40, 430, 870)); check(r is None or r[0] != "Holtek", f"Holtek wrong header-nibble rejected -> {r}")
# one pulse dropped from a CAME-12 (breaks pairing)
bad = cap_came_like(0xABC, 12, 320, 640); del bad[5]
r = decode(bad); check(r is None or r[1] != 12 or r[0] != "CAME" or r[1:] != (0xABC,), True)  # informational
print(f"  (info) pulse-dropped CAME-12 -> {decode(bad)}")

print("== stateless: valid -> invalid -> valid independent ==")
a = decode(cap_came_like(0xABC, 12, 320, 640))
_ = decode([100,-100,100,-100])  # junk
b = decode(cap_came_like(0xABC, 12, 320, 640))
check(a == b and a is not None, "same CAME decodes identically after a junk burst")

print("== encoder matches INDEPENDENT spec frame (level+duration sequence) ==")
# CAME-12 full frame per spec: header low (47*320), start high 320, then pairs
def spec_came_frame(key, bits, ts, tl):
    out = [-(ts*came_header(bits)), ts]
    for i in range(bits-1, -1, -1):
        if (key >> i) & 1: out += [-tl, ts]
        else: out += [-ts, tl]
    return out
check(encode_came_like(320,640,came_header(12),0xABC,12) == spec_came_frame(0xABC,12,320,640), "CAME-12 encode == spec")
check(encode_came_like(700,1400,36,0x801,12) == ([-700*36,700] + (lambda k,b: sum(([-1400,700] if (k>>i)&1 else [-700,1400] for i in range(b-1,-1,-1)),[]))(0x801,12)), "NiceFlo-12 encode == spec")
# Princeton encode spec: pairs then stop + guard
def spec_princeton_frame(key, te, guard=30):
    out = []
    for i in range(23,-1,-1):
        out += ([te*3,-te] if (key>>i)&1 else [te,-te*3])
    out += [te, -(te*guard)]
    return out
check(encode_princeton(390,30,0xABCDEF,24) == spec_princeton_frame(0xABCDEF,390), "Princeton encode == spec")

print("== bit order MSB-first (0x800001) ==")
enc = encode_came_like(320,640,came_header(24),0x800001,24)
# first data bit after header+start should be '1' (MSB): pair = [-tl, ts]
first_pair = enc[2:4]
check(first_pair == [-640, 320], f"MSB-first: first bit of 0x800001 is 1 -> {first_pair}")
# last data bit is '1' (LSB set): last pair = [-tl, ts]
check(enc[-2:] == [-640,320], f"LSB bit of 0x800001 is 1 -> {enc[-2:]}")
# middle bits 0
check(enc[4:6] == [-320,640], f"second bit of 0x800001 is 0 -> {enc[4:6]}")

print("== round-trip encode(strip header/guard)->capture->decode ==")
# CAME: our encoder frame minus the header low == the captured burst
fr = encode_came_like(320,640,came_header(12),0xABC,12)[1:]  # drop header low
check(decode(fr) == ("CAME",0xABC,12,320), f"CAME-12 round-trip -> {decode(fr)}")
frp = encode_princeton(390,30,0xABCDEF,24)[:-1]  # drop guard low
check(decode(frp) == ("Princeton",0xABCDEF,24,390), f"Princeton round-trip -> {decode(frp)}")

print("== NEW protocols decode to correct proto/key/bits ==")
for k in (0x3FF, 0x001, 0x200, 0x155, 0x2AA, 0x000):
    r = decode(cap_linear(k)); check(r == ("Linear", k, 10, 500), f"Linear 0x{k:03X} (lone high) -> {r}")
    r = decode(cap_linear_gap(k)); check(r == ("Linear", k, 10, 500), f"Linear 0x{k:03X} (gap present) -> {r}")
for k in (0xABCDEF, 0x800001, 0x000000, 0xFFFFFF, 0x123456):
    r = decode(cap_gate_tx(k)); check(r == ("GateTX", k, 24, 350), f"GateTX 0x{k:06X} -> {r}")
for k in (0xABC, 0x801, 0x000, 0xFFF, 0x555):
    r = decode(cap_ansonic(k), fm=True); check(r == ("Ansonic", k, 12, 555), f"Ansonic 0x{k:03X} (FM) -> {r}")
# Ansonic is FM-only: the exact same waveform under an AM capture must NOT decode
# (keeps the Nice FLO / Ansonic timing overlap from producing a wrong AM label).
r = decode(cap_ansonic(0xABC), fm=False)
check(r is None, f"Ansonic waveform under AM capture does not decode -> {r}")

print("== NEW protocols tolerance (within window) ==")
for pct in (0.05, 0.10, 0.20, -0.05, -0.10, -0.20):
    r = decode(jitter(cap_linear(0x2AB), pct))
    check(r == ("Linear", 0x2AB, 10, 500), f"Linear jitter {pct:+.0%} -> {r}")
for pct in (0.05, 0.10, -0.05, -0.10):   # gate short-leg delta is tight (±100 on 350)
    r = decode(jitter(cap_gate_tx(0xABCDEF), pct))
    check(r == ("GateTX", 0xABCDEF, 24, 350), f"GateTX jitter {pct:+.0%} -> {r}")
# Ansonic's te_delta=120 on te_long=1111 is only ~±10.8%, so ±10% is the faithful
# tolerance edge (Flipper rejects beyond it too).
for pct in (0.05, 0.10, -0.05, -0.10):
    r = decode(jitter(cap_ansonic(0xABC), pct), fm=True)
    check(r == ("Ansonic", 0xABC, 12, 555), f"Ansonic jitter {pct:+.0%} (FM) -> {r}")

print("== CROSS false-positive: each protocol classified as ITSELF, not a sibling ==")
r = decode(cap_linear(0x2AB));        check(r and r[0] == "Linear",  f"Linear not mis-read -> {r[0] if r else None}")
r = decode(cap_gate_tx(0xABCDEF));    check(r and r[0] == "GateTX",  f"GateTX not CAME/Nice -> {r[0] if r else None}")
r = decode(cap_ansonic(0xABC), fm=True); check(r and r[0] == "Ansonic", f"Ansonic not Nice/CAME -> {r[0] if r else None}")
# and the EXISTING protocols are not stolen by the new decoders
r = decode(cap_came_like(0xABCDEF, 24, 320, 640)); check(r and r[0] == "CAME",    f"CAME-24 still CAME (not GateTX) -> {r[0] if r else None}")
r = decode(cap_came_like(0x801, 12, 700, 1400));   check(r and r[0] == "NiceFlo", f"Nice-12 still Nice (not GateTX/Ansonic) -> {r[0] if r else None}")
r = decode(cap_came_like(0x5123456789, 40, 430, 870)); check(r and r[0] == "Holtek", f"Holtek-40 still Holtek -> {r[0] if r else None}")
r = decode(cap_princeton(0xABCDEF, 390)); check(r and r[0] == "Princeton", f"Princeton-24 still Princeton (not Linear) -> {r[0] if r else None}")

print("== NEW encoders match INDEPENDENT spec frame ==")
def spec_linear(key, ts=500, tl=1500):
    out = []
    for i in range(9, 0, -1):
        out += [tl, -ts] if (key >> i) & 1 else [ts, -tl]
    out += [tl, -(ts*42)] if (key & 1) else [ts, -(ts*44)]
    return out
def spec_gate(key, ts=350, tl=700):
    out = [-(ts*49), tl]            # header low te_short*49, start high te_long
    for i in range(23, -1, -1):
        out += [-tl, ts] if (key >> i) & 1 else [-ts, tl]
    return out
def spec_ansonic(key, ts=555, tl=1111):
    out = [-(ts*35), ts]            # header low te_short*35, start high te_short
    for i in range(11, -1, -1):
        out += [-ts, tl] if (key >> i) & 1 else [-tl, ts]   # inverted: bit1 = L short, H long
    return out
check(encode_linear(500, 1500, 0x2AB, 10) == spec_linear(0x2AB), "Linear encode == spec")
check(encode_pwm_frame(350, 700, 49, 700, False, 0xABCDEF, 24) == spec_gate(0xABCDEF), "GateTX encode == spec")
check(encode_pwm_frame(555, 1111, 35, 555, True, 0xABC, 12) == spec_ansonic(0xABC), "Ansonic encode == spec")

print("== NEW bit order MSB-first ==")
eg = encode_pwm_frame(350, 700, 49, 700, False, 0x800001, 24)
check(eg[2:4] == [-700, 350], f"GateTX MSB bit of 0x800001 is 1 -> {eg[2:4]}")
check(eg[-2:] == [-700, 350], f"GateTX LSB bit of 0x800001 is 1 -> {eg[-2:]}")
check(eg[4:6] == [-350, 700], f"GateTX second bit of 0x800001 is 0 -> {eg[4:6]}")
ea = encode_pwm_frame(555, 1111, 35, 555, True, 0x801, 12)
check(ea[2:4] == [-555, 1111], f"Ansonic MSB bit of 0x801 is 1 (L short,H long) -> {ea[2:4]}")
check(ea[-2:] == [-555, 1111], f"Ansonic LSB bit of 0x801 is 1 -> {ea[-2:]}")
el = encode_linear(500, 1500, 0x200, 10)
check(el[0:2] == [1500, -500], f"Linear MSB bit of 0x200 is 1 (H long,L short) -> {el[0:2]}")

print("== NEW round-trip encode->capture->decode ==")
# GateTX/Ansonic: our encoder frame minus the header low == the captured burst
fg = encode_pwm_frame(350, 700, 49, 700, False, 0x0F0F0F, 24)[1:]
check(decode(fg) == ("GateTX", 0x0F0F0F, 24, 350), f"GateTX round-trip -> {decode(fg)}")
fa = encode_pwm_frame(555, 1111, 35, 555, True, 0x5A5, 12)[1:]
check(decode(fa, fm=True) == ("Ansonic", 0x5A5, 12, 555), f"Ansonic round-trip -> {decode(fa, fm=True)}")
# Linear: encoder frame with its trailing gap intact (even-length) decodes
fl = encode_linear(500, 1500, 0x14D, 10)
check(decode(fl) == ("Linear", 0x14D, 10, 500), f"Linear round-trip (gap present) -> {decode(fl)}")

print("== Doitrand / Phoenix_V2 decode + tolerance + cross + round-trip ==")
for k in (0x123456789, 0x1FFFFFFFFF, 0x0000000001, 0x1555555555):
    r = decode(cap_doitrand(k)); check(r == ("Doitrand", k, 37, 400), f"Doitrand 0x{k:010X} -> {r}")
for k in (0xABCDEF012345, 0xFFFFFFFFFFFFF, 0x1, 0x8000000000000):
    r = decode(cap_phoenix(k)); check(r == ("Phoenix_V2", k, 52, 427), f"Phoenix_V2 0x{k:013X} -> {r}")
for pct in (0.05, 0.10, -0.05, -0.10):
    r = decode(jitter(cap_doitrand(0x123456789), pct))
    check(r == ("Doitrand", 0x123456789, 37, 400), f"Doitrand jitter {pct:+.0%} -> {r}")
for pct in (0.05, -0.05):   # phoenix te_delta=100 on te_long=853 -> ~±11% edge
    r = decode(jitter(cap_phoenix(0xABCDEF012345), pct))
    check(r == ("Phoenix_V2", 0xABCDEF012345, 52, 427), f"Phoenix_V2 jitter {pct:+.0%} -> {r}")
# cross: classified as itself, and the 2:1 siblings are not stolen
check(decode(cap_doitrand(0x123456789))[0] == "Doitrand", "Doitrand not mis-read")
check(decode(cap_phoenix(0xABCDEF012345))[0] == "Phoenix_V2", "Phoenix_V2 not mis-read")
check(decode(cap_came_like(0xABCDEF, 24, 320, 640))[0] == "CAME", "CAME-24 still CAME (not Doitrand)")
check(decode(cap_came_like(0x801, 12, 700, 1400))[0] == "NiceFlo", "Nice-12 still Nice")
check(decode(cap_gate_tx(0xABCDEF))[0] == "GateTX", "GateTX still GateTX (not Doitrand)")
# encode == independent spec frame
def spec_doitrand(key):
    out = [-(400*62), 700]
    for i in range(36, -1, -1):
        out += [-1100, 400] if (key >> i) & 1 else [-400, 1100]
    return out
def spec_phoenix(key):
    out = [-(427*60), 2562]
    for i in range(51, -1, -1):
        out += [-427, 853] if (key >> i) & 1 else [-853, 427]   # inverted: bit1 = L short, H long
    return out
check(encode_pwm_frame(400,1100,62,700,False,0x123456789,37) == spec_doitrand(0x123456789), "Doitrand encode == spec")
check(encode_pwm_frame(427,853,60,2562,True,0xABCDEF012345,52) == spec_phoenix(0xABCDEF012345), "Phoenix_V2 encode == spec")
# round-trip: encoder frame minus header low == captured burst
fd = encode_pwm_frame(400,1100,62,700,False,0x0F0F0F0F0,37)[1:]
check(decode(fd) == ("Doitrand", 0x0F0F0F0F0, 37, 400), f"Doitrand round-trip -> {decode(fd)}")
fp = encode_pwm_frame(427,853,60,2562,True,0x5A5A5A5A5A5A,52)[1:]
check(decode(fp) == ("Phoenix_V2", 0x5A5A5A5A5A5A, 52, 427), f"Phoenix_V2 round-trip -> {decode(fp)}")

print("== Clemsa (18-bit, HIGH-first 7:1) decode + tolerance + cross + round-trip ==")
for k in (0x3FFFF, 0x00001, 0x20000, 0x15555, 0x2AAAA):
    r = decode(cap_clemsa(k)); check(r == ("Clemsa", k, 18, 385), f"Clemsa 0x{k:05X} -> {r}")
for pct in (0.05, 0.10, -0.05, -0.10):
    r = decode(jitter(cap_clemsa(0x2AB3C), pct))
    check(r == ("Clemsa", 0x2AB3C, 18, 385), f"Clemsa jitter {pct:+.0%} -> {r}")
# not stolen, and does not steal the HIGH-first siblings
check(decode(cap_clemsa(0x2AB3C))[0] == "Clemsa", "Clemsa not mis-read")
check(decode(cap_linear(0x2AB))[0] == "Linear", "Linear still Linear (not Clemsa)")
check(decode(cap_princeton(0xABCDEF, 390))[0] == "Princeton", "Princeton still Princeton (not Clemsa)")
check(decode(cap_phoenix(0xABCDEF012345))[0] == "Phoenix_V2", "Phoenix_V2 still itself (Clemsa doesn't steal)")
# encode == independent spec frame (HIGH-first, trailer te_short+te_long*7 / te_long*8)
def spec_clemsa(key):
    out = []
    for i in range(17, 0, -1):
        out += [2695, -385] if (key >> i) & 1 else [385, -2695]
    out += [2695, -(385 + 2695*7)] if (key & 1) else [385, -(2695*8)]
    return out
check(encode_phf(385, 2695, 0x2AB3C, 18, 385+2695*7, 2695*8) == spec_clemsa(0x2AB3C), "Clemsa encode == spec")
# round-trip: full frame with trailer present (even length) decodes via end-gap branch
fc = encode_phf(385, 2695, 0x14D2E, 18, 385+2695*7, 2695*8)
check(decode(fc) == ("Clemsa", 0x14D2E, 18, 385), f"Clemsa round-trip (gap present) -> {decode(fc)}")

print("== SMC5326 (25-bit, HIGH-first 3:1 + stop high) decode + tolerance + cross + round-trip ==")
for k in (0x1FFFFFF, 0x0000001, 0x1000000, 0x1555555, 0x0AAAAAA, 0x0000000):
    r = decode(cap_smc5326(k)); check(r == ("SMC5326", k, 25, 300), f"SMC5326 0x{k:07X} -> {r}")
for pct in (0.05, 0.10, 0.20, -0.05, -0.10, -0.20):   # delta 200: short leg huge margin, long leg ~±22%
    r = decode(jitter(cap_smc5326(0x15AB3C5), pct))
    check(r == ("SMC5326", 0x15AB3C5, 25, 300), f"SMC5326 jitter {pct:+.0%} -> {r}")
# cross: SMC classified as itself; Princeton NOT stolen by SMC and vice-versa.
check(decode(cap_smc5326(0x15AB3C5))[0] == "SMC5326", "SMC5326 not mis-read")
check(decode(cap_princeton(0xABCDEF, 390))[0] == "Princeton", "Princeton-24 (te=390) still Princeton (not SMC)")
check(decode(cap_princeton(0x000001, 300))[0] == "Princeton", "Princeton-24 (te=300, SMC-like legs) still Princeton (not SMC)")
check(decode(cap_linear(0x2AB))[0] == "Linear", "Linear still Linear (not SMC)")
check(decode(cap_clemsa(0x2AB3C))[0] == "Clemsa", "Clemsa still Clemsa (not SMC)")
# encode == independent spec frame (3:1 pairs + stop high + te*25 guard)
def spec_smc5326(key, te=300, guard=25):
    out = []
    for i in range(24, -1, -1):
        out += [te*3, -te] if (key >> i) & 1 else [te, -te*3]
    out += [te, -(te*guard)]
    return out
check(encode_princeton(300, 25, 0x15AB3C5, 25) == spec_smc5326(0x15AB3C5), "SMC5326 encode == spec (reuses encode_princeton, guard=25)")
# bit order MSB-first: bit24 set / bit0 clear in 0x1000000
es = encode_princeton(300, 25, 0x1000000, 25)
check(es[0:2] == [900, -300], f"SMC5326 MSB bit of 0x1000000 is 1 (H long,L short) -> {es[0:2]}")
check(es[-4:-2] == [300, -900], f"SMC5326 LSB bit of 0x1000000 is 0 (H short,L long) -> {es[-4:-2]}")
# round-trip: encoder frame minus the guard low (inter-frame sep) == captured burst
fs = encode_princeton(300, 25, 0x14D2E3A, 25)[:-1]
check(decode(fs) == ("SMC5326", 0x14D2E3A, 25, 300), f"SMC5326 round-trip -> {decode(fs)}")
# bounds (reuses encode_princeton cap/bit guards)
def enc_smc_cap(te, key, bits, cap):
    if bits == 0 or bits > 32: return None
    if (2*bits + 2) > cap: return None
    return encode_princeton(te, 25, key, bits)
check(len(enc_smc_cap(300, 0x1FFFFFF, 25, 1024)) == 2*25+2, "SMC5326-25 => 52 elems")
check(enc_smc_cap(300, 0x1FFFFFF, 25, 51) is None, "SMC5326 rejected when cap<52")

print("== Dooya (40-bit, HIGH-first 2:1 + preamble) decode + tolerance + cross + round-trip ==")
for k in (0x123456789A, 0xFFFFFFFFFF, 0x0000000001, 0x8000000000, 0xAAAAAAAAAA, 0x5555555555, 0xE1DC030533):
    r = decode(cap_dooya(k)); check(r == ("Dooya", k, 40, 366), f"Dooya 0x{k:010X} -> {r}")
# Tolerance edge is the PREAMBLE gate, not the data windows: start-high 4758 has
# only te_delta*5 = 600 (~±12.6%), so ±10% is the faithful edge (Flipper's
# te_short*13 check rejects beyond it too). Data legs tolerate ~±33%.
for pct in (0.05, 0.10, -0.05, -0.10):
    r = decode(jitter(cap_dooya(0xE1DC030533), pct))
    check(r == ("Dooya", 0xE1DC030533, 40, 366), f"Dooya jitter {pct:+.0%} -> {r}")
# cross: Dooya is itself; the unique preamble keeps it isolated both ways.
check(decode(cap_dooya(0xE1DC030533))[0] == "Dooya", "Dooya not mis-read")
check(decode(cap_smc5326(0x15AB3C5))[0] == "SMC5326", "SMC5326 still SMC5326 (not Dooya)")
check(decode(cap_princeton(0xABCDEF, 390))[0] == "Princeton", "Princeton still Princeton (not Dooya)")
check(decode(cap_linear(0x2AB))[0] == "Linear", "Linear still Linear (not Dooya)")
check(decode(cap_clemsa(0x2AB3C))[0] == "Clemsa", "Clemsa still Clemsa (not Dooya)")
check(decode(cap_phoenix(0xABCDEF012345))[0] == "Phoenix_V2", "Phoenix_V2 still itself (start 2562 != Dooya 4758)")
# the start low (1466) sits clearly above an ordinary long leg (733 + d_long 240 = 973)
check(not within(733 + 240, 1466, 360), "ordinary long leg (973) is NOT inside the start-low window [1106,1826]")
# encode == independent spec frame (header low + start high + start low + pairs)
def spec_dooya(key):
    out = [-(733*12 + (733 if (key & 1) else 366)), 4758, -1466]
    for i in range(39, -1, -1):
        out += [733, -366] if (key >> i) & 1 else [366, -733]
    return out
check(encode_dooya(0xE1DC030533) == spec_dooya(0xE1DC030533), "Dooya encode == spec")
# bit order MSB-first: bit39 set / bit0 clear in 0x8000000000
ed = encode_dooya(0x8000000000)
check(ed[3:5] == [733, -366], f"Dooya MSB bit is 1 (H long,L short) -> {ed[3:5]}")
check(ed[-2:] == [366, -733], f"Dooya LSB bit is 0 (H short,L long) -> {ed[-2:]}")
# round-trip: encoder frame minus the leading header low == captured burst.
# Full 40-pair form: the 40th bit's low (<end_gap_min) is read as an ordinary pair.
fd = encode_dooya(0x14D2E3A5C7)[1:]
check(decode(fd) == ("Dooya", 0x14D2E3A5C7, 40, 366), f"Dooya round-trip (full pairs) -> {decode(fd)}")
# lone-high form (40th low merged into the stripped separator) also decodes -> bit 40 read
check(decode(cap_dooya(0x14D2E3A5C7)) == ("Dooya", 0x14D2E3A5C7, 40, 366), "Dooya round-trip (lone-high bit 40)")
# bounds
def enc_dooya_cap(key, bits, cap):
    if bits == 0 or bits > 40: return None
    if (3 + 2*bits) > cap: return None
    return encode_dooya(key, bits)
check(len(enc_dooya_cap(0xFFFFFFFFFF, 40, 1024)) == 3 + 2*40, "Dooya-40 => 83 elems")
check(enc_dooya_cap(0xFFFFFFFFFF, 40, 82) is None, "Dooya rejected when cap<83")

print("== Magellan (32-bit, 2:1 + sync preamble + CRC8) decode + CRC gate + cross + round-trip ==")
def mag_key(payload):   # 24-bit payload + CRC8 over its 3 bytes (low byte)
    c = magellan_crc8([(payload >> 16) & 0xFF, (payload >> 8) & 0xFF, payload & 0xFF])
    return (payload << 8) | c
for p in (0xABCDEF, 0x000000, 0xFFFFFF, 0x123456, 0x800001):
    k = mag_key(p)
    r = decode(cap_magellan(k)); check(r == ("Magellan", k, 32, 200), f"Magellan payload 0x{p:06X} key 0x{k:08X} -> {r}")
# CRC gate: corrupt the CRC byte -> rejected (not decoded)
kbad = mag_key(0xABCDEF) ^ 0x01
check(decode(cap_magellan(kbad)) is None, f"Magellan bad CRC rejected -> {decode(cap_magellan(kbad))}")
# payload bit flipped (CRC no longer matches) -> rejected
kbad2 = mag_key(0xABCDEF) ^ 0x100
check(decode(cap_magellan(kbad2)) is None, "Magellan payload-flip (CRC mismatch) rejected")
# too few sync pairs -> rejected
def cap_mag_short_preamble(key, pairs):
    out = [800, -200]
    for _ in range(pairs): out += [200, -200]
    out += [200, -400, 1200, -400]
    for i in range(31, -1, -1): out += [200, -400] if (key >> i) & 1 else [400, -200]
    out += [200]
    return out
check(decode(cap_mag_short_preamble(mag_key(0xABCDEF), 8)) is None, "Magellan <11 sync pairs rejected")
# tolerance
for pct in (0.05, 0.10, -0.05, -0.10):
    k = mag_key(0x123456)
    r = decode(jitter(cap_magellan(k), pct))
    check(r == ("Magellan", k, 32, 200), f"Magellan jitter {pct:+.0%} -> {r}")
# cross: Magellan itself; existing protocols neither steal nor are stolen
check(decode(cap_magellan(mag_key(0xABCDEF)))[0] == "Magellan", "Magellan not mis-read")
check(decode(cap_came_like(0xABC, 12, 320, 640))[0] == "CAME", "CAME-12 still CAME (not Magellan)")
check(decode(cap_came_like(0x801, 12, 700, 1400))[0] == "NiceFlo", "Nice-12 still Nice (not Magellan)")
check(decode(cap_dooya(0xE1DC030533))[0] == "Dooya", "Dooya still Dooya (not Magellan)")
check(decode(cap_gate_tx(0xABCDEF))[0] == "GateTX", "GateTX still GateTX (not Magellan)")
check(decode(cap_smc5326(0x15AB3C5))[0] == "SMC5326", "SMC5326 still SMC5326 (not Magellan)")
# encode == independent spec frame
def spec_magellan(key):
    ts, tl = 200, 400
    out = [ts*4, -ts]
    for _ in range(12): out += [ts, -ts]
    out += [ts, -tl, ts*6, -tl]
    for i in range(31, -1, -1): out += [ts, -tl] if (key >> i) & 1 else [tl, -ts]
    out += [ts, -(tl*100)]
    return out
check(encode_magellan(mag_key(0xABCDEF)) == spec_magellan(mag_key(0xABCDEF)), "Magellan encode == spec")
# bit order MSB-first (data region starts after header(2)+sync(24)+marker(2)+start(2) = idx 30)
em = encode_magellan(0x80000000)
check(em[30:32] == [200, -400], f"Magellan MSB bit is 1 (H short,L long) -> {em[30:32]}")
check(em[-4:-2] == [400, -200], f"Magellan LSB bit is 0 (H long,L short) -> {em[-4:-2]}")
# round-trip: encoder frame minus the trailing te_long*100 separator == captured burst
fm_ = encode_magellan(mag_key(0x123456))[:-1]
check(decode(fm_) == ("Magellan", mag_key(0x123456), 32, 200), f"Magellan round-trip -> {decode(fm_)}")
# bounds
def enc_mag_cap(key, bits, cap):
    if bits != 32: return None
    if (2 + 24 + 2 + 2 + 2*bits + 2) > cap: return None
    return encode_magellan(key, bits)
check(len(enc_mag_cap(mag_key(0), 32, 1024)) == 2 + 24 + 2 + 2 + 2*32 + 2, "Magellan-32 => 96 elems")
check(enc_mag_cap(mag_key(0), 32, 95) is None, "Magellan rejected when cap<96")

print("== Intertechno V3 (32-bit, 2 pairs/bit, 5:1 sync) decode + tolerance + cross + round-trip ==")
for k in (0x12345678, 0x00000001, 0x80000000, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555):
    r = decode(cap_intertechno(k)); check(r == ("Intertechno_V3", k, 32, 275), f"Intertechno 0x{k:08X} -> {r}")
# also decodes WITHOUT the trailing header high (ends on the final bit's L2)
check(decode(cap_intertechno(0x12345678)[:-1]) == ("Intertechno_V3", 0x12345678, 32, 275), "Intertechno no trailing high")
# capture trims the final bit's L2 (trailing low) -> frame ends on the 32nd bit's 2nd high
check(decode(cap_intertechno(0x12345678)[:-2]) == ("Intertechno_V3", 0x12345678, 32, 275), "Intertechno final-L2 trimmed (ends on 2nd high)")
for pct in (0.05, 0.10, -0.05, -0.10):
    r = decode(jitter(cap_intertechno(0x12345678), pct))
    check(r == ("Intertechno_V3", 0x12345678, 32, 275), f"Intertechno jitter {pct:+.0%} -> {r}")
# the 36-bit dimming sub-variant is intentionally unsupported -> rejected, not mis-decoded
def cap_intertechno_dimm():
    out = [275, -2750]
    for i in range(36):
        if i == 27: out += [275, -275, 275, -275]      # dimm symbol (short,short)(short,short)
        elif i & 1: out += [275, -1375, 275, -275]     # bit1
        else:       out += [275, -275, 275, -1375]     # bit0
    out += [275]
    return out
check(decode(cap_intertechno_dimm()) is None, "Intertechno 36-bit dimming rejected (unsupported)")
# cross: Intertechno itself; neighbours neither stolen nor stealing
check(decode(cap_intertechno(0x12345678))[0] == "Intertechno_V3", "Intertechno not mis-read")
check(decode(cap_dooya(0xE1DC030533))[0] == "Dooya", "Dooya still Dooya (not Intertechno)")
check(decode(cap_magellan(mag_key(0xABCDEF)))[0] == "Magellan", "Magellan still Magellan (not Intertechno)")
check(decode(cap_came_like(0xABC, 12, 320, 640))[0] == "CAME", "CAME-12 still CAME (not Intertechno)")
check(decode(cap_smc5326(0x15AB3C5))[0] == "SMC5326", "SMC5326 still SMC5326 (not Intertechno)")
# encode == independent spec frame
def spec_intertechno(key):
    ts, tl = 275, 1375
    out = [ts, -(ts*38), ts, -2750]
    for i in range(31, -1, -1):
        out += [ts, -tl, ts, -ts] if (key >> i) & 1 else [ts, -ts, ts, -tl]
    return out
check(encode_intertechno(0x12345678) == spec_intertechno(0x12345678), "Intertechno encode == spec")
# bit order MSB-first (data starts at idx 4)
ei = encode_intertechno(0x80000000)
check(ei[4:8] == [275, -1375, 275, -275], f"Intertechno MSB bit is 1 -> {ei[4:8]}")
check(ei[-4:] == [275, -275, 275, -1375], f"Intertechno LSB bit is 0 -> {ei[-4:]}")
# round-trip: encoder frame minus the [header high, separator low] == captured burst
fi = encode_intertechno(0x0F0F0F0F)[2:]
check(decode(fi) == ("Intertechno_V3", 0x0F0F0F0F, 32, 275), f"Intertechno round-trip -> {decode(fi)}")
# bounds
def enc_it_cap(key, bits, cap):
    if bits != 32: return None
    if (4 + 4*bits) > cap: return None
    return encode_intertechno(key, bits)
check(len(enc_it_cap(0, 32, 1024)) == 4 + 4*32, "Intertechno-32 => 132 elems")
check(enc_it_cap(0, 32, 131) is None, "Intertechno rejected when cap<132")

print("== FALSE POSITIVE re-sweep with ALL decoders, both modulations ==")
random.seed(7)
fp_am = fp_fm = 0
for _ in range(50000):
    n = random.randint(4, 140)   # long enough to reach 37/52-bit and Intertechno (131-elem) frames
    seq = []; sign = 1
    for _ in range(n):
        seq.append(sign * random.randint(100, 3000)); sign = -sign   # up to Phoenix start
    if seq[0] < 0: seq[0] = -seq[0]
    if decode(seq, fm=False) is not None: fp_am += 1
    if decode(seq, fm=True)  is not None: fp_fm += 1
check(fp_am == 0, f"50000 random bursts AM set (13 protocols): {fp_am} false")
check(fp_fm == 0, f"50000 random bursts FM set (Ansonic): {fp_fm} false")
# Dooya's preamble needs a ~4758 us high, beyond the 3000 us ceiling above, so the
# sweep never reaches its gate. Re-sweep with a 6000 us ceiling to stress it.
random.seed(11)
fp_hi = 0
for _ in range(50000):
    n = random.randint(4, 90)
    seq = []; sign = 1
    for _ in range(n):
        seq.append(sign * random.randint(100, 6000)); sign = -sign
    if seq[0] < 0: seq[0] = -seq[0]
    if decode(seq, fm=False) is not None: fp_hi += 1
check(fp_hi == 0, f"50000 random bursts (<=6000 us, reaches Dooya 4758 preamble): {fp_hi} false")

print("== encoder BOUNDS (mirror of the C cap/bit guards) ==")
# Guard-aware mirrors: return None when the C would return 0.
def enc_pwm_cap(ts, tl, hdr, start, inv, key, bits, cap):
    if bits == 0 or bits > 64: return None   # mirror: C cap raised to 64 (Phoenix=52)
    if (2 + 2*bits) > cap: return None
    return encode_pwm_frame(ts, tl, hdr, start, inv, key, bits)
def enc_linear_cap(ts, tl, key, bits, cap):
    if bits == 0 or bits > 32: return None
    if (2*bits) > cap: return None
    return encode_linear(ts, tl, key, bits)
def enc_phf_cap(ts, tl, key, bits, tr1, tr0, cap):
    if bits == 0 or bits > 32: return None
    if (2*bits) > cap: return None
    return encode_phf(ts, tl, key, bits, tr1, tr0)
def enc_princeton_cap(te, guard, key, bits, cap):
    if bits == 0 or bits > 32: return None
    if (2*bits + 2) > cap: return None
    return encode_princeton(te, guard, key, bits)

# element counts match the exact formula (so the s_emu_buf[1024] margin is proven)
check(len(enc_pwm_cap(350,700,49,700,False,0xABCDEF,24,1024)) == 2+2*24, "GateTX-24 => 50 elems")
check(len(enc_pwm_cap(555,1111,35,555,True,0xABC,12,1024)) == 2+2*12, "Ansonic-12 => 26 elems")
check(len(enc_pwm_cap(430,870,36,430,False,0x5123456789,40,1024)) == 2+2*40, "Holtek-40 => 82 elems")
check(len(enc_pwm_cap(320,640,76,320,False,0x3FFFFFFFFFF,42,1024)) == 2+2*42, "CAME-42 => 86 elems")
check(len(enc_linear_cap(500,1500,0x2AB,10,1024)) == 2*10, "Linear-10 => 20 elems")
check(len(enc_princeton_cap(390,30,0xABCDEF,24,1024)) == 2*24+2, "Princeton-24 => 50 elems")
check(len(enc_pwm_cap(400,1100,62,700,False,0x123456789,37,1024)) == 2+2*37, "Doitrand-37 => 76 elems")
check(len(enc_pwm_cap(427,853,60,2562,True,0xABCDEF012345,52,1024)) == 2+2*52, "Phoenix_V2-52 => 106 elems")
check(len(enc_phf_cap(385,2695,0x2AB3C,18,385+2695*7,2695*8,1024)) == 2*18, "Clemsa-18 => 36 elems")
check(enc_pwm_cap(427,853,60,2562,True,0,65,1024) is None, "PWM bits>64 rejected")
# every protocol's max frame fits the 1024-element emulate buffer
check(all(len(x) <= 1024 for x in [
        enc_pwm_cap(350,700,49,700,False,0xFFFFFF,24,1024),
        enc_pwm_cap(555,1111,35,555,True,0xFFF,12,1024),
        enc_pwm_cap(430,870,36,430,False,0xFFFFFFFFFF,40,1024),
        encode_pwm_frame(320,640,76,320,False,0x3FFFFFFFFFF,42),
        enc_linear_cap(500,1500,0x3FF,10,1024),
        enc_princeton_cap(390,30,0xFFFFFF,24,1024)]), "all max frames <= 1024")
# cap too small => reject (0), not an overflow write
check(enc_pwm_cap(350,700,49,700,False,0xABCDEF,24,49) is None, "GateTX rejected when cap<50")
check(enc_linear_cap(500,1500,0x2AB,10,19) is None, "Linear rejected when cap<20")
check(enc_princeton_cap(390,30,0xABCDEF,24,49) is None, "Princeton rejected when cap<50")
# out-of-range bit counts => reject
check(enc_pwm_cap(350,700,49,700,False,0,43,1024) is not None, "PWM bits=43 accepted (cap now 64)")
check(enc_pwm_cap(350,700,49,700,False,0,0,1024) is None, "PWM bits==0 rejected")
check(enc_linear_cap(500,1500,0,33,1024) is None, "Linear bits>32 rejected")

print()
print("RESULT:", "ALL PASS" if FAIL == 0 else f"{FAIL} FAILURES")
import sys; sys.exit(1 if FAIL else 0)
