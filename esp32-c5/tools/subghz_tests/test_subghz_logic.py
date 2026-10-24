#!/usr/bin/env python3
# Logic mirrors of: (1) the Stage 6b brute worker index/resume/bounds, and
# (2) the subghz_store.c strict .sub parser. Validates the algorithm logic the
# audit flagged (resume definition, no skip/dup, strict rejection) with a mock
# TX / synthetic text. Not the compiled binary.

FAIL = 0
def check(cond, msg):
    global FAIL
    if cond: print(f"  PASS {msg}")
    else:
        FAIL += 1; print(f"  FAIL {msg}")

# ---------- brute worker mirror ----------
MAX_BITS = 20
def brute_start_validate(bits):
    # returns (ok, end) mirroring subghz_brute_start
    if bits <= 0 or bits > MAX_BITS:
        return (False, None)
    return (True, (1 << bits) - 1)

def brute_run(idx, end, repeats, stop_at_top=None):
    # s_brute_idx = NEXT code to send. stop_at_top = code value at whose loop-top
    # s_run becomes false (simulates a clean stop between codes).
    sent = []
    code = idx
    running = True
    while code <= end and running:
        if stop_at_top is not None and code == stop_at_top:
            running = False
            break
        for _r in range(repeats):
            sent.append(code)
        idx = code + 1          # resume point = next code
        code += 1
    status = "DONE" if idx > end else "STOPPED"
    return sent, idx, status

print("== brute bit-size validation ==")
check(brute_start_validate(12) == (True, 4095), "CAME 12 bits -> 0..4095")
check(brute_start_validate(20) == (True, (1<<20)-1), "20 bits -> 0..1048575 (max)")
check(brute_start_validate(24)[0] is False, "Princeton 24 bits rejected (>20)")
check(brute_start_validate(40)[0] is False, "Holtek 40 bits rejected (>20)")
check(brute_start_validate(0)[0] is False, "0 bits rejected")

print("== brute full run / done ==")
sent, idx, st = brute_run(0, 9, 1)
check(sent == list(range(10)) and idx == 10 and st == "DONE", f"0..9 full -> sent={len(sent)} idx={idx} {st}")
sent, idx, st = brute_run(0, 3, 2)
check(sent == [0,0,1,1,2,2,3,3] and idx == 4 and st == "DONE", f"repeats=2 -> {sent} idx={idx} {st}")

print("== brute stop + resume: no skip / no double ==")
s1, idx1, st1 = brute_run(0, 9, 1, stop_at_top=4)   # stop before code 4
check(s1 == [0,1,2,3] and idx1 == 4 and st1 == "STOPPED", f"stop@4 -> sent={s1} idx={idx1} {st1}")
s2, idx2, st2 = brute_run(idx1, 9, 1)                # resume
check(s2 == [4,5,6,7,8,9] and idx2 == 10 and st2 == "DONE", f"resume -> sent={s2} idx={idx2} {st2}")
combined = s1 + s2
check(combined == list(range(10)), "stop+resume covers every code exactly once (no skip/dup)")

print("== brute boundary: last code + progress-64 edge ==")
sent, idx, st = brute_run(0, 64, 1)   # 0..64 inclusive = 65 codes, crosses the &0x3F==0 points
check(sent == list(range(65)) and idx == 65 and st == "DONE", f"0..64 -> {len(sent)} codes, idx={idx} {st}")
check(len(set(sent)) == 65, "no duplicate codes across the 64-progress boundary")

# ---------- strict .sub parser mirror ----------
MAX_DURATION = 1000000
def parse_sub(text, cap):
    # returns ("ok", freq, preset, vals) or ("err", reason, None, None)
    # Frequency mandatory + nonzero + range
    fi = text.find("Frequency:")
    if fi < 0: return ("err", "no-freq", None, None)
    tok = text[fi+10:].split()[0] if text[fi+10:].split() else ""
    try:
        freq = int(tok)
    except ValueError:
        return ("err", "freq-nan", None, None)
    if freq == 0 or freq > 0xFFFFFFFF: return ("err", "freq-bad", None, None)
    preset = ""
    pi = text.find("Preset:")
    if pi >= 0:
        rest = text[pi+7:].strip().split()
        if rest: preset = rest[0]
    vals = []
    idx = 0
    while True:
        ri = text.find("RAW_Data:", idx)
        if ri < 0: break
        nl = text.find("\n", ri)
        line = text[ri+9: nl if nl >= 0 else len(text)]
        for t in line.split():
            try:
                v = int(t)
            except ValueError:
                return ("err", "token-nan", None, None)
            if v == 0 or abs(v) > MAX_DURATION:
                return ("err", "token-range", None, None)
            if len(vals) >= cap:
                return ("err", "overflow", None, None)
            vals.append(v)
        if nl < 0: break
        idx = nl + 1
    if len(vals) < 2: return ("err", "too-few", None, None)
    return ("ok", freq, preset, vals)

CAP = 1024
print("== parser: valid single + multiline ==")
ok = parse_sub("Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: 433920000\n"
               "Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n"
               "RAW_Data: 350 -700 350 -1400 700 -350\n", CAP)
check(ok[0]=="ok" and ok[1]==433920000 and ok[3]==[350,-700,350,-1400,700,-350], f"valid -> {ok[:3]} n={len(ok[3])}")
ml = parse_sub("Frequency: 315000000\nPreset: X\nProtocol: RAW\nRAW_Data: 100 -100 200\nRAW_Data: -200 300 -300\n", CAP)
check(ml[0]=="ok" and ml[3]==[100,-100,200,-200,300,-300], f"multiline accumulates in order -> {ml[3]}")

print("== parser: strict rejections (no silent skip) ==")
check(parse_sub("Preset: X\nRAW_Data: 100 -100 200\n", CAP)[1]=="no-freq", "missing Frequency rejected")
check(parse_sub("Frequency: 0\nRAW_Data: 100 -100\n", CAP)[1]=="freq-bad", "zero Frequency rejected")
check(parse_sub("Frequency: 433920000\nRAW_Data: 100 xyz -100\n", CAP)[1]=="token-nan", "non-numeric token rejects file")
check(parse_sub("Frequency: 433920000\nRAW_Data: 100 0 -100\n", CAP)[1]=="token-range", "zero duration rejects file")
check(parse_sub("Frequency: 433920000\nRAW_Data: 100 2000000 -100\n", CAP)[1]=="token-range", "over-range duration rejected")
check(parse_sub("Frequency: 433920000\nRAW_Data: 100\n", CAP)[1]=="too-few", "<2 values rejected")
big = "Frequency: 433920000\nRAW_Data: " + " ".join(["100","-100"]*600) + "\n"  # 1200 > cap 1024
check(parse_sub(big, CAP)[1]=="overflow", "more than cap tokens rejected (not truncated)")

print()
print("RESULT:", "ALL PASS" if FAIL == 0 else f"{FAIL} FAILURES")
import sys; sys.exit(1 if FAIL else 0)
