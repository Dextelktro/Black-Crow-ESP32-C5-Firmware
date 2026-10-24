#!/usr/bin/env python3
# Logic mirror of the Stage 7 Key File path in subghz_store.c:
# build_key_file / subghz_store_classify / subghz_store_load_key.
# Validates save->classify->load round-trip, 8-byte MSB-first key parsing
# (incl. high-bit-set and leading-zero keys), and RAW-vs-KEY classification.

FAIL = 0
def check(cond, msg):
    global FAIL
    if cond: print(f"  PASS {msg}")
    else:
        FAIL += 1; print(f"  FAIL {msg}")

def build_key_file(proto, bits, key, te, freq, preset):
    s = ("Filetype: Flipper SubGhz Key File\nVersion: 1\n"
         f"Frequency: {freq}\nPreset: {preset}\nProtocol: {proto}\nBit: {bits}\n")
    s += "Key:" + "".join(f" {(key >> (b*8)) & 0xFF:02X}" for b in range(7, -1, -1))
    s += f"\nTE: {te}\n"
    return s

def classify(text):
    i = text.find("Protocol:")
    if i < 0: return "NONE"
    t = text[i+9:].lstrip(" \t")
    if t[:3].upper() == "RAW" and (len(t) <= 3 or t[3] in " \r\n"):
        return "RAW"
    return "KEY"

def parse_ul_strict(s):
    """Mirror of subghz_store.c parse_ul_strict(): leading base-10 uint whose
    only trailing content is whitespace / EOL / end. None on junk like '12x'."""
    s = s.lstrip(" \t")
    j = 0
    while j < len(s) and s[j].isdigit(): j += 1
    if j == 0: return None
    rest = s[j:].lstrip(" \t")
    if rest and rest[0] not in "\r\n": return None
    return int(s[:j])

def load_key(text):
    fi = text.find("Frequency:")
    if fi < 0: return None
    freq = parse_ul_strict(text[fi+10:])
    if freq is None or freq == 0: return None
    pi = text.find("Protocol:")
    proto = text[pi+9:].split()[0] if pi >= 0 else ""
    bi = text.find("Bit:")
    if bi < 0: return None
    bits = parse_ul_strict(text[bi+4:])
    if bits is None or bits == 0 or bits > 64: return None
    ki = text.find("Key:")
    if ki < 0: return None
    nl = text.find("\n", ki)
    line = text[ki+4: nl if nl >= 0 else len(text)]
    k = 0; nb = 0
    for tok in line.split():
        if nb >= 8: break
        b = int(tok, 16)
        if b < 0 or b > 0xFF: return None
        k = (k << 8) | (b & 0xFF); nb += 1
    if nb == 0: return None
    ti = text.find("TE:")
    if ti >= 0:
        te = parse_ul_strict(text[ti+3:])
        if te is None: return None       # present but malformed -> reject
    else:
        te = 0
    return proto, bits, k, te, freq

print("== Key File save -> classify -> load round-trip ==")
cases = [
    ("Princeton", 24, 0xABCDEF, 390, 433920000),
    ("CAME", 12, 0xABC, 0, 433920000),
    ("Holtek", 40, 0x5123456789, 430, 433920000),      # 40-bit
    ("NiceFlo", 12, 0x001, 0, 868350000),              # leading zeros
    ("CAME", 42, 0x3FFFFFFFFFF, 0, 433920000),         # 42-bit max
    ("Holtek", 40, 0xFF00000000, 430, 915000000),      # high byte set
]
for proto, bits, key, te, freq in cases:
    txt = build_key_file(proto, bits, key, te, 433920000 if freq == 433920000 else freq,
                         "FuriHalSubGhzPresetOok650Async")
    k = classify(txt)
    r = load_key(txt)
    ok = (k == "KEY" and r is not None and r[0] == proto and r[1] == bits
          and r[2] == key and r[3] == te)
    check(ok, f"{proto} bits={bits} key=0x{key:X} te={te} -> classify={k} load={r}")

print("== 64-bit key with top bit set survives exactly ==")
txt = build_key_file("Holtek", 40, 0xABCDEF0123, 430, 433920000, "P")
r = load_key(txt)
check(r is not None and r[2] == 0xABCDEF0123, f"key 0xABCDEF0123 -> 0x{(r[2] if r else 0):X}")

print("== classify RAW vs KEY vs NONE ==")
raw = ("Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\n"
       "Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\nRAW_Data: 350 -700 350\n")
check(classify(raw) == "RAW", "RAW file -> RAW")
check(classify(build_key_file("CAME", 12, 0xABC, 0, 433920000, "P")) == "KEY", "Key file -> KEY")
check(classify("Filetype: x\nFrequency: 1\n") == "NONE", "no Protocol line -> NONE")

print("== load_key rejects malformed ==")
check(load_key("Protocol: CAME\nBit: 12\nKey: 00 01\n") is None, "missing Frequency -> reject")
check(load_key("Frequency: 433920000\nProtocol: CAME\nKey: 00 01\n") is None, "missing Bit -> reject")
check(load_key("Frequency: 433920000\nProtocol: CAME\nBit: 0\nKey: 00\n") is None, "Bit=0 -> reject")

print("== load_key STRICT: trailing garbage in numeric fields rejected ==")
check(load_key("Frequency: 433920000\nProtocol: CAME\nBit: 12x\nKey: 00 01\n") is None,
      "Bit: 12x (trailing junk) -> reject")
check(load_key("Frequency: 433920000\nProtocol: CAME\nBit: 12\nKey: 00 01\nTE: 390abc\n") is None,
      "TE: 390abc (trailing junk) -> reject")
check(load_key("Frequency: 433920000x\nProtocol: CAME\nBit: 12\nKey: 00 01\n") is None,
      "Frequency: 433920000x (trailing junk) -> reject")
check(load_key("Frequency: 433920000 \nProtocol: CAME\nBit: 12 \nKey: 00 01\nTE: 390 \n") is not None,
      "trailing spaces before EOL -> accepted")
check(load_key("Frequency: 433920000\nProtocol: CAME\nBit: 12\nKey: 00 01\n") is not None,
      "no TE line (optional) -> accepted")

print()
print("RESULT:", "ALL PASS" if FAIL == 0 else f"{FAIL} FAILURES")
import sys; sys.exit(1 if FAIL else 0)
