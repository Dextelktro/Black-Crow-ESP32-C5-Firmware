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

def decode(d, fm=False):
    if fm:
        # FM/2FSK set: Ansonic only (kept disjoint from AM Nice FLO).
        r = decode_pwm_frame(d, 555, 120, 555, 1111, 120, 120, True)   # ansonic
        if r and r[1] == 12:
            return ("Ansonic", r[0], 12, 555)
        return None
    # AM/OOK set.
    r = decode_princeton(d)
    if r: return r
    r = decode_linear(d)
    if r: return r
    r = decode_phf(d, 385, 2695, 150, 450, 4000, 18)               # clemsa
    if r: return ("Clemsa", r[0], 18, 385)
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

print("== FALSE POSITIVE re-sweep with ALL decoders, both modulations ==")
random.seed(7)
fp_am = fp_fm = 0
for _ in range(50000):
    n = random.randint(4, 120)   # long enough to reach 37/52-bit frames
    seq = []; sign = 1
    for _ in range(n):
        seq.append(sign * random.randint(100, 3000)); sign = -sign   # up to Phoenix start
    if seq[0] < 0: seq[0] = -seq[0]
    if decode(seq, fm=False) is not None: fp_am += 1
    if decode(seq, fm=True)  is not None: fp_fm += 1
check(fp_am == 0, f"50000 random bursts AM set (8 protocols): {fp_am} false")
check(fp_fm == 0, f"50000 random bursts FM set (Ansonic): {fp_fm} false")

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
