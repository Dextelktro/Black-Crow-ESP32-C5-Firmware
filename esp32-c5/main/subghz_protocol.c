/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 * SPDX-FileCopyrightText: Flipper Devices Inc. and the flipperzero-firmware contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contains material derived from the GPL-3.0 Flipper Zero firmware; see NOTICE.
 */

#include "subghz_protocol.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

#include "subghz_capture.h"

/* Supported protocol pulse legs are at least 320 us. This software-only
 * decoder cleanup can therefore repair isolated sub-50 us spikes without
 * changing the RAW waveform retained for inspection or saving. */
#define SUBGHZ_DECODE_GLITCH_US 50

static int32_t s_decode_durations[SUBGHZ_RAW_MAX_TIMINGS];

static uint64_t duration_magnitude(int32_t duration)
{
    return duration < 0 ? (uint64_t)(-(int64_t)duration) : (uint64_t)duration;
}

static bool remove_short_glitches(const int32_t *durations, size_t count,
                                  int32_t *out, size_t *out_count)
{
    if (count > SUBGHZ_RAW_MAX_TIMINGS) return false;
    memcpy(out, durations, count * sizeof(*out));

    size_t n = count;
    for (size_t i = 1; i + 1 < n;) {
        int32_t before = out[i - 1];
        int32_t glitch = out[i];
        int32_t after = out[i + 1];
        uint64_t before_us = duration_magnitude(before);
        uint64_t glitch_us = duration_magnitude(glitch);
        uint64_t after_us = duration_magnitude(after);

        if (glitch_us < SUBGHZ_DECODE_GLITCH_US &&
            before_us >= SUBGHZ_DECODE_GLITCH_US &&
            after_us >= SUBGHZ_DECODE_GLITCH_US &&
            ((before < 0) == (after < 0)) &&
            ((before < 0) != (glitch < 0))) {
            uint64_t merged_us = before_us + glitch_us + after_us;
            if (merged_us > INT32_MAX) return false;

            out[i - 1] = before < 0 ? -(int32_t)merged_us : (int32_t)merged_us;
            memmove(&out[i], &out[i + 2], (n - i - 2) * sizeof(*out));
            n -= 2;
            if (i > 1) i--;
        } else {
            i++;
        }
    }
    *out_count = n;
    return true;
}

/* Faithful constants from Flipper Zero Sub-GHz protocol sources (dev branch).
 * te_short/te_long in microseconds; deltas are the decoder match tolerances. */
#define PRINCETON_TE_SHORT   390     /* encoder default Te (us) */
#define PRINCETON_BITS       24
#define PRINCETON_GUARD      30      /* guard low = guard * te */
/* NOTE: Princeton decode uses a 3:1 RATIO gate (see decode_princeton), not
 * absolute short/long windows like the 2:1 protocols, so there are no
 * PRINCETON_TE_LONG / delta constants here. */

#define CAME_TE_SHORT        320
#define CAME_TE_LONG         640
#define CAME_DELTA           150

#define NICE_TE_SHORT        700
#define NICE_TE_LONG         1400
#define NICE_DELTA           200

#define HOLTEK_TE_SHORT      430
#define HOLTEK_TE_LONG       870
#define HOLTEK_DELTA         100
#define HOLTEK_BITS          40
#define HOLTEK_HEADER_MASK   0xF000000000ULL
#define HOLTEK_HEADER        0x5000000000ULL

/* Linear (lib/subghz/protocols/linear.c): te_short=500, te_long=1500,
 * te_delta=350, exactly 10 bits. H-then-L pairs, 3:1 like Princeton, but NO
 * start bit; the frame-end gap (te_short*42/44) follows the last bit's high.
 * NOTE on the key value: Flipper stores the raw data bits in the `.sub` Key
 * File (interop-compatible with what we store/encode here), but its UI DISPLAYS
 * the one's-complement (~data) for historical remote compatibility. So the app
 * layer, not this decoder, is where a Linear key should be shown inverted to
 * match a Flipper screen; the on-air waveform and stored value are the raw
 * bits. */
#define LINEAR_TE_SHORT      500
#define LINEAR_TE_LONG       1500
#define LINEAR_DELTA         350
#define LINEAR_BITS          10
#define LINEAR_END_GAP_MIN   2500    /* te_short*5: Flipper frame-end threshold */
#define LINEAR_END_GAP_TE1   42      /* last bit 1 -> trailer low = te_short*42 */
#define LINEAR_END_GAP_TE0   44      /* last bit 0 -> trailer low = te_short*44 */

/* Gate TX (lib/subghz/protocols/gate_tx.c): te_short=350, te_long=700, 24 bits.
 * L-then-H pairs (same polarity as CAME), but the start bit is te_long (not
 * te_short) and the header low is te_short*49. Bit tolerance ±100 on the short
 * leg, ±300 on the long leg (matches Flipper's asymmetric windows). */
#define GATE_TX_TE_SHORT     350
#define GATE_TX_TE_LONG      700
#define GATE_TX_DELTA        100     /* short-leg / bit tolerance */
#define GATE_TX_START_DELTA  300     /* start + long-leg tolerance (te_long±300) */
#define GATE_TX_BITS         24
#define GATE_TX_HEADER_TE    49      /* header low = te_short*49 */

/* Ansonic (lib/subghz/protocols/ansonic.c): te_short=555, te_long=1111,
 * te_delta=120, 12 bits, FM. L-then-H pairs but INVERTED polarity (bit1 = L
 * short + H long). Start bit = te_short, header low = te_short*35. */
#define ANSONIC_TE_SHORT     555
#define ANSONIC_TE_LONG      1111
#define ANSONIC_DELTA        120
#define ANSONIC_BITS         12
#define ANSONIC_HEADER_TE    35      /* header low = te_short*35 */

/* Doitrand (lib/subghz/protocols/doitrand.c): te_short=400, te_long=1100,
 * te_delta=150, 37 bits, AM/433. CAME-like 2:1 (L-H), bit1 = L long + H short.
 * Header low = te_short*62 (24.8 ms, segments cleanly). Encoder start high is
 * 2*te_short-100 = 700; the decoder centers the start at 2*te_short = 800. The
 * long leg needs its own wider delta (ratio 1100/400 = 2.75, not 2:1). */
#define DOITRAND_TE_SHORT    400
#define DOITRAND_TE_LONG     1100
#define DOITRAND_D_SHORT     150
#define DOITRAND_D_LONG      450
#define DOITRAND_BITS        37
#define DOITRAND_HEADER_TE   62
#define DOITRAND_START       700     /* encoder emits 2*te_short - 100 */
#define DOITRAND_START_REF   800     /* decoder centers 2*te_short */
#define DOITRAND_START_DELTA 450

/* Phoenix_V2 (lib/subghz/protocols/phoenix_v2.c): te_short=427, te_long=853,
 * te_delta=100, 52 bits, AM/433. 2:1 (L-H) INVERTED (bit1 = L short + H long).
 * Header low = te_short*60 (25.6 ms); start high = te_short*6 = 2562 (a
 * distinctive wide start that no other protocol here shares). */
#define PHOENIX_TE_SHORT     427
#define PHOENIX_TE_LONG      853
#define PHOENIX_D_SHORT      100
#define PHOENIX_D_LONG       300
#define PHOENIX_BITS         52
#define PHOENIX_HEADER_TE    60
#define PHOENIX_START        (427 * 6)   /* 2562 */
#define PHOENIX_START_DELTA  400

/* Clemsa (lib/subghz/protocols/clemsa.c — BERNER/ELKA/TEDSEN/TELETASTER):
 * te_short=385, te_long=2695 (7:1!), te_delta=150, 18 bits, AM/433. Same shape
 * as Linear (HIGH-then-LOW pairs, no start bit, last bit's low is the
 * inter-frame gap), but a 7:1 ratio and asymmetric leg tolerance (short ±150,
 * long ±450). Trailer low = te_short+te_long*7 (bit1) or te_long*8 (bit0),
 * ~19-21 ms, so it segments cleanly. */
#define CLEMSA_TE_SHORT      385
#define CLEMSA_TE_LONG       2695
#define CLEMSA_D_SHORT       150
#define CLEMSA_D_LONG        450
#define CLEMSA_BITS          18
#define CLEMSA_END_GAP_MIN   4000    /* > te_long+delta (3145), < trailer (19250) */
#define CLEMSA_TRAILER1      (CLEMSA_TE_SHORT + CLEMSA_TE_LONG * 7)  /* 19250 */
#define CLEMSA_TRAILER0      (CLEMSA_TE_LONG * 8)                    /* 21560 */

/* SMC5326 (lib/subghz/protocols/smc5326.c — AX5326/PT226x tri-state DIP family):
 * te_short=300, te_long=900 (3:1), te_delta=200, 25 bits, AM/OOK. Same HIGH-first
 * shape as Linear/Clemsa (bit1 = H long + L short, bit0 = H short + L long),
 * followed by a lone STOP high (te_short). The te_short*25 guard low that ends
 * the word is the inter-frame separator (7500 us > SUBGHZ_FRAME_GAP_US), so
 * capture strips it and the delivered frame is [25x(H,L)] + [stop high]. The
 * tri-state DIP label is a display concern only; the stored value is the raw 25
 * bits. te is fixed (not per-capture estimated). The encoder is byte-identical
 * in shape to Princeton's (3:1 pairs + stop high + te*guard low), so it reuses
 * encode_princeton with guard = 25 (vs Princeton's 30). END_GAP_MIN must exceed
 * te_long so a bit-0 long low (900) is read as an ordinary pair, not a frame end. */
#define SMC5326_TE_SHORT     300
#define SMC5326_TE_LONG      900
#define SMC5326_DELTA        200     /* faithful te_delta, both legs */
#define SMC5326_BITS         25
#define SMC5326_END_GAP_MIN  1800    /* te_long*2: Flipper word-end threshold */
#define SMC5326_GUARD        25      /* guard low = te_short*25 (inter-frame gap) */

/* Dooya (lib/subghz/protocols/dooya.c — curtain/blind motors): te_short=366,
 * te_long=733 (2:1), te_delta=120, 40 bits, AM (433/315). HIGH-first 2:1 like
 * Linear/SMC (bit1 = H long + L short, bit0 = H short + L long), but preceded by
 * a two-element preamble: start high te_short*13 (4758) + low te_long*2 (1466).
 * The ~9 ms header low that precedes the start high (te_long*12 + te_long|te_short)
 * is the inter-frame separator — capture strips it, so a delivered frame starts
 * on the start high. The 40th bit's low merges into the next frame's header, so
 * it arrives as a lone trailing high (decode_pwm_high_first handles that).
 * FAITHFUL deltas are ASYMMETRIC: dooya.c classifies the short leg with te_delta
 * (120) and the long leg with te_delta*2 (240). The start low (te_long*2 = 1466)
 * sits well above an ordinary long leg (te_long + te_delta*2 = 973), so the
 * preamble gate can never be confused with a data pair. */
#define DOOYA_TE_SHORT          366
#define DOOYA_TE_LONG           733
#define DOOYA_D_SHORT           120     /* te_delta (short leg) */
#define DOOYA_D_LONG            240     /* te_delta*2 (long leg, faithful) */
#define DOOYA_BITS              40
#define DOOYA_PREAMBLE_HI       (366 * 13)   /* 4758 = te_short*13 (start high) */
#define DOOYA_PREAMBLE_HI_DELTA (120 * 5)    /* 600  = te_delta*5 */
#define DOOYA_PREAMBLE_LO       (733 * 2)    /* 1466 = te_long*2 (start low) */
#define DOOYA_PREAMBLE_LO_DELTA (120 * 3)    /* 360  = te_delta*3 */
#define DOOYA_END_GAP_MIN       (733 * 4)    /* 2932: > te_long(733), < header (~9 ms) */
#define DOOYA_HEADER_TE         12           /* header low = te_long*12 + te_long|te_short */

/* Magellan (lib/subghz/protocols/magellan.c — security sensors): te_short=200,
 * te_long=400 (2:1), te_delta=100, 32 bits (24-bit payload + 8-bit CRC), AM/433.
 * Frame: a sync-pulse train (>10 pairs of short H + short L), a (short H, long L)
 * marker, a (te_short*6 H, long L) start bit, then 32 HIGH-first 2:1 bits INVERTED
 * vs Dooya (bit1 = H short + L long, bit0 = H long + L short), then a stop high
 * (short); the te_long*100 (40 ms) low after it is the inter-frame separator
 * (stripped by capture, so the stop arrives as a lone trailing high). The low
 * byte is a CRC-8/0x31 over the upper three bytes — a strong self-validation gate,
 * so this decoder runs last in the AM set. The encoder's te_short*4 header high is
 * not a sync pulse; the decoder (like Flipper) syncs on the first short high. */
#define MAGELLAN_TE_SHORT       200
#define MAGELLAN_TE_LONG        400
#define MAGELLAN_DELTA          100
#define MAGELLAN_BITS           32
#define MAGELLAN_HEADER_MIN     10           /* require > 10 sync pairs (Flipper: > 10) */
#define MAGELLAN_START_HI       (200 * 6)    /* 1200 = te_short*6 = te_long*3 (start high) */

/* Intertechno V3 (lib/subghz/protocols/intertechno_v3.c — HomeEasy/Elro/etc home
 * switches): te_short=275, te_long=1375 (5:1), te_delta=150, 32 bits, AM. Each
 * logical bit is TWO (high, low) pairs with a constant te_short high; the lows
 * carry the data: bit1 = (short H, long L)(short H, short L), bit0 = (short H,
 * short L)(short H, long L). Frame: a short-high + te_short*10 (2750) sync low,
 * then the 32 two-pair bits; the ~10.4 ms (te_short*38) inter-frame low is the
 * separator (> SUBGHZ_RX_MAX_NS, so each frame is its own reception), stripped by
 * capture, leaving a trailing short high (the next frame's header) that the
 * decoder tolerates. The 36-bit dimming sub-variant is NOT supported here (a
 * 36-bit frame is rejected rather than mis-decoded); only the common 32-bit
 * remote. Key is a clean 32-bit value. */
#define INTERTECHNO_TE_SHORT    275
#define INTERTECHNO_TE_LONG     1375
#define INTERTECHNO_DELTA       150
#define INTERTECHNO_BITS        32
#define INTERTECHNO_SYNC_LO     (275 * 10)   /* 2750 = te_short*10 (sync low) */
#define INTERTECHNO_SEP_TE      38           /* inter-frame low = te_short*38 (encoder) */

static inline bool within(uint32_t a, uint32_t target, uint32_t delta)
{
    uint32_t d = a > target ? a - target : target - a;
    return d < delta;
}

const char *subghz_protocol_name(subghz_proto_id_t id)
{
    switch (id) {
        case SUBGHZ_PROTO_PRINCETON: return "Princeton";
        case SUBGHZ_PROTO_LINEAR:    return "Linear";
        case SUBGHZ_PROTO_CAME:      return "CAME";
        case SUBGHZ_PROTO_NICE_FLO:  return "NiceFlo";
        case SUBGHZ_PROTO_HOLTEK:    return "Holtek";
        case SUBGHZ_PROTO_GATE_TX:   return "GateTX";
        case SUBGHZ_PROTO_ANSONIC:   return "Ansonic";
        case SUBGHZ_PROTO_DOITRAND:  return "Doitrand";
        case SUBGHZ_PROTO_PHOENIX_V2: return "Phoenix_V2";
        case SUBGHZ_PROTO_CLEMSA:    return "Clemsa";
        case SUBGHZ_PROTO_SMC5326:   return "SMC5326";
        case SUBGHZ_PROTO_DOOYA:     return "Dooya";
        case SUBGHZ_PROTO_MAGELLAN:  return "Magellan";
        case SUBGHZ_PROTO_INTERTECHNO: return "Intertechno_V3";
        default:                     return "unknown";
    }
}

subghz_proto_id_t subghz_protocol_from_name(const char *name)
{
    if (!name) return SUBGHZ_PROTO_UNKNOWN;
    for (int i = 1; i < SUBGHZ_PROTO_COUNT; i++) {
        if (strcasecmp(name, subghz_protocol_name((subghz_proto_id_t)i)) == 0) {
            return (subghz_proto_id_t)i;
        }
    }
    /* EV1527 / PT2262 / HS1527 / RT1527 are the SAME 24-bit 3:1 OOK waveform as
     * Princeton — a single received frame cannot be told apart from Princeton,
     * and Flipper labels the whole learning-code family "Princeton" (its .sub
     * files store that name). So decode reports and save as "Princeton" for
     * interop, but we accept these common aliases as names here so an
     * EV1527-named key (hand-entered, or a .sub from another tool) resolves to
     * the Princeton codec for emulate/transmit. */
    if (strcasecmp(name, "EV1527") == 0 || strcasecmp(name, "PT2262") == 0 ||
        strcasecmp(name, "HS1527") == 0 || strcasecmp(name, "RT1527") == 0) {
        return SUBGHZ_PROTO_PRINCETON;
    }
    /* Holtek HT12X (12-bit) is byte-identical on-air to CAME-12 (start high
     * te_short, 12 pairs at 320/640, no distinguishing header or checksum); the
     * only difference is the preamble length, which capture strips. A single
     * frame therefore cannot be told apart from CAME-12, so there is deliberately
     * no separate decoder (it would only steal CAME-12). Accept the names so an
     * HT12X-labelled key (hand-entered or a .sub from another tool) resolves to
     * the CAME codec for emulate/transmit; decode and save report "CAME". Same
     * rationale as the EV1527 aliases above. */
    if (strcasecmp(name, "HT12X") == 0 || strcasecmp(name, "Holtek_HT12X") == 0) {
        return SUBGHZ_PROTO_CAME;
    }
    return SUBGHZ_PROTO_UNKNOWN;
}

bool subghz_protocol_is_fm(subghz_proto_id_t id)
{
    return id == SUBGHZ_PROTO_ANSONIC;   /* only FM protocol in the set */
}

uint16_t subghz_protocol_default_bits(subghz_proto_id_t id)
{
    switch (id) {
        case SUBGHZ_PROTO_PRINCETON: return PRINCETON_BITS;
        case SUBGHZ_PROTO_LINEAR:    return LINEAR_BITS;
        case SUBGHZ_PROTO_CAME:      return 12;
        case SUBGHZ_PROTO_NICE_FLO:  return 12;
        case SUBGHZ_PROTO_HOLTEK:    return HOLTEK_BITS;
        case SUBGHZ_PROTO_GATE_TX:   return GATE_TX_BITS;
        case SUBGHZ_PROTO_ANSONIC:   return ANSONIC_BITS;
        case SUBGHZ_PROTO_DOITRAND:  return DOITRAND_BITS;
        case SUBGHZ_PROTO_PHOENIX_V2: return PHOENIX_BITS;
        case SUBGHZ_PROTO_CLEMSA:    return CLEMSA_BITS;
        case SUBGHZ_PROTO_SMC5326:   return SMC5326_BITS;
        case SUBGHZ_PROTO_DOOYA:     return DOOYA_BITS;
        case SUBGHZ_PROTO_MAGELLAN:  return MAGELLAN_BITS;
        case SUBGHZ_PROTO_INTERTECHNO: return INTERTECHNO_BITS;
        default:                     return 0;
    }
}

/* ---------------------------- decode ---------------------------- */

/* Princeton: pairs of (+high, -low); bit1 = H long + L short, bit0 = H short +
 * L long (3:1). 24 bits, no start bit, trailing stop high (+ guard low, which
 * ended the burst and is trimmed). Estimates Te from the frame. */
static bool decode_princeton(const int32_t *d, size_t count,
                             uint64_t *out_data, int *out_bits, uint32_t *out_te)
{
    uint64_t data = 0;
    int bits = 0;
    uint64_t total = 0;
    uint32_t te_ref = 0;   /* FIX: short-leg referensi dari pair pertama */
    size_t i = 0;
    while (i + 1 < count && bits < PRINCETON_BITS) {
        int32_t high = d[i], low = d[i + 1];
        if (high <= 0 || low >= 0) break;
        uint32_t ha = (uint32_t)high, la = (uint32_t)(-low);
        uint32_t mn = ha < la ? ha : la;
        uint32_t mx = ha < la ? la : ha;
        if (mn < 80 || mn > 2000) break;
        if (mx < mn * 2 + mn / 3 || mx > mn * 4) break;
        /* FIX: tolak drift Te >33% antar-pair. Remote asli stabil dalam satu
         * frame (~37ms); noise acak biasanya tidak. Sebelumnya mn divalidasi
         * lepas [80,2000]us per-pair tanpa kaitan satu sama lain, jadi noise
         * bisa lolos 24 pair dan ke-decode jadi "Princeton" palsu. */
        if (te_ref == 0) {
            te_ref = mn;
        } else {
            uint32_t tol = te_ref / 3;
            if (mn > te_ref + tol || mn < te_ref - tol) break;
        }
        int bit = (ha > la) ? 1 : 0;
        data = (data << 1) | (uint64_t)bit;
        total += ha + la;
        bits++;
        i += 2;
    }
    if (bits != PRINCETON_BITS) return false;
    *out_data = data & 0xFFFFFFULL;
    *out_bits = PRINCETON_BITS;
    *out_te = (uint32_t)(total / (PRINCETON_BITS * 4));
    return true;
}

/* PWM-framed 2:1 family (CAME / Nice FLO / Holtek / Gate-TX / Ansonic):
 * [+start, then (-low, +high) pairs]. A "long-low" pair (L long + H short) is
 * bit 1 and a "short-low" pair (L short + H long) is bit 0 for the non-inverted
 * protocols; `inverted` swaps that (Ansonic). The start bit is matched against
 * `start_ref ± start_delta` (te_short for most, te_long for Gate-TX). Returns
 * raw data + bit count; the caller validates the count (and any header nibble).
 */
static bool decode_pwm_frame(const int32_t *d, size_t count,
                             uint32_t start_ref, uint32_t start_delta,
                             uint32_t ts, uint32_t tl,
                             uint32_t d_short, uint32_t d_long,
                             bool inverted,
                             uint64_t *out_data, int *out_bits)
{
    if (count < 3 || d[0] <= 0) return false;
    if (!within((uint32_t)d[0], start_ref, start_delta)) return false;  /* start bit */
    uint64_t data = 0;
    int bits = 0;
    size_t i = 1;
    while (i + 1 < count) {
        int32_t low = d[i], high = d[i + 1];
        if (low >= 0 || high <= 0) break;
        uint32_t la = (uint32_t)(-low), ha = (uint32_t)high;
        int bit;
        if (within(la, tl, d_long) && within(ha, ts, d_short)) {
            bit = inverted ? 0 : 1;          /* long-low */
        } else if (within(la, ts, d_short) && within(ha, tl, d_long)) {
            bit = inverted ? 1 : 0;          /* short-low */
        } else {
            break;
        }
        if (bits < 64) data = (data << 1) | (uint64_t)bit;
        bits++;
        i += 2;
    }
    *out_data = data;
    *out_bits = bits;
    return true;
}

/* HIGH-first PWM family (Linear, Clemsa, Princeton-shaped): H-then-L pairs, no
 * start bit, bit1 = H long + L short, bit0 = H short + L long. The final bit's
 * low leg is the inter-frame gap, which capture strips (frame ends on a mark) —
 * so the last bit is classified by its lone high (or an explicit end gap >=
 * end_gap_min). Accepts ONLY when the whole frame is consumed as exactly
 * `want_bits` bits, so a frame of a different length/shape cannot be mis-read.
 * Short/long legs take their own deltas (ratios here are 3:1 and 7:1). */
static bool decode_pwm_high_first(const int32_t *d, size_t count,
                                  uint32_t ts, uint32_t tl,
                                  uint32_t d_short, uint32_t d_long,
                                  uint32_t end_gap_min, int want_bits,
                                  uint64_t *out_data, int *out_bits)
{
    uint64_t data = 0;
    int bits = 0;
    size_t i = 0;
    while (i < count && bits < want_bits) {
        int32_t hi = d[i];
        if (hi <= 0) return false;                       /* must be a mark */
        uint32_t ha = (uint32_t)hi;
        bool long_hi  = within(ha, tl, d_long);
        bool short_hi = within(ha, ts, d_short);
        if (!long_hi && !short_hi) return false;

        if (i + 1 < count && d[i + 1] < 0) {
            uint32_t la = (uint32_t)(-d[i + 1]);
            if (la < end_gap_min) {                      /* ordinary bit pair */
                bool long_lo  = within(la, tl, d_long);
                bool short_lo = within(la, ts, d_short);
                int bit;
                if (long_hi && short_lo)      bit = 1;
                else if (short_hi && long_lo) bit = 0;
                else return false;
                data = (data << 1) | (uint64_t)bit;
                bits++;
                i += 2;
                continue;
            }
            /* frame-end gap: this high is the last bit, classified alone */
            data = (data << 1) | (uint64_t)(long_hi ? 1 : 0);
            bits++;
            i += 2;
            break;
        }

        /* lone trailing high (gap stripped by capture): last bit */
        data = (data << 1) | (uint64_t)(long_hi ? 1 : 0);
        bits++;
        i += 1;
        break;
    }
    if (bits != want_bits) return false;
    if (i != count) return false;                        /* whole frame consumed */
    *out_data = data;
    *out_bits = bits;
    return true;
}

/* Linear: 10-bit, 3:1 (te_short=500/te_long=1500), symmetric delta 350. */
static bool decode_linear(const int32_t *d, size_t count,
                          uint64_t *out_data, int *out_bits)
{
    if (!decode_pwm_high_first(d, count, LINEAR_TE_SHORT, LINEAR_TE_LONG,
                               LINEAR_DELTA, LINEAR_DELTA, LINEAR_END_GAP_MIN,
                               LINEAR_BITS, out_data, out_bits)) {
        return false;
    }
    *out_data &= 0x3FFULL;
    return true;
}

/* SMC5326: 25-bit HIGH-first 3:1 (like Linear/Clemsa) plus a trailing stop high.
 * The delivered frame is [25x(H,L)] + [stop H]; the te_short*25 guard low that
 * follows ended the word and is the inter-frame separator, stripped by capture.
 * Decode the 25 pairs with the shared HIGH-first primitive (whose exact-bit +
 * whole-consume guard is the anti-collision vs Princeton-24 — a 24-pair frame
 * can never yield 25 bits), then require the lone stop high (te_short). This
 * wrapper is tried BEFORE decode_princeton: Princeton does not require whole-frame
 * consumption, so it would otherwise read an SMC frame's first 24 pairs as a
 * false Princeton-24. */
static bool decode_smc5326(const int32_t *d, size_t count,
                           uint64_t *out_data, int *out_bits)
{
    if (count < 2) return false;
    int32_t stop = d[count - 1];
    if (stop <= 0 || !within((uint32_t)stop, SMC5326_TE_SHORT, SMC5326_DELTA)) {
        return false;                       /* trailing stop high (te_short) */
    }
    if (!decode_pwm_high_first(d, count - 1, SMC5326_TE_SHORT, SMC5326_TE_LONG,
                               SMC5326_DELTA, SMC5326_DELTA, SMC5326_END_GAP_MIN,
                               SMC5326_BITS, out_data, out_bits)) {
        return false;
    }
    *out_data &= 0x1FFFFFFULL;              /* 25 bits */
    return true;
}

/* Dooya: a start-high (te_short*13) + start-low (te_long*2) preamble, then 40
 * HIGH-first 2:1 bits. The ~9 ms header low that precedes the start high is the
 * inter-frame separator (stripped by capture), so the frame starts on the start
 * high; the 40th bit's low merged into that separator, so it arrives as a lone
 * trailing high. Gate the two preamble elements explicitly (the start low sits
 * far above an ordinary long leg, so no data pair can satisfy it), then decode
 * the body with the shared HIGH-first primitive (whole-frame consume + exact 40
 * bits). The long-leg delta is te_delta*2 (faithful to dooya.c). */
static bool decode_dooya(const int32_t *d, size_t count,
                         uint64_t *out_data, int *out_bits)
{
    if (count < 4) return false;
    if (d[0] <= 0 ||
        !within((uint32_t)d[0], DOOYA_PREAMBLE_HI, DOOYA_PREAMBLE_HI_DELTA)) {
        return false;                       /* start high = te_short*13 */
    }
    if (d[1] >= 0 ||
        !within((uint32_t)(-d[1]), DOOYA_PREAMBLE_LO, DOOYA_PREAMBLE_LO_DELTA)) {
        return false;                       /* start low = te_long*2 (>> long leg) */
    }
    if (!decode_pwm_high_first(d + 2, count - 2, DOOYA_TE_SHORT, DOOYA_TE_LONG,
                               DOOYA_D_SHORT, DOOYA_D_LONG, DOOYA_END_GAP_MIN,
                               DOOYA_BITS, out_data, out_bits)) {
        return false;
    }
    *out_data &= 0xFFFFFFFFFFULL;           /* 40 bits */
    return true;
}

/* Magellan CRC-8 (poly 0x31, init 0x00, MSB-first), faithful to magellan.c. */
static uint8_t magellan_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Magellan: sync-pulse train (>10 short/short pairs) + (short,long) marker +
 * (te_short*6, long) start bit + 32 HIGH-first 2:1 bits (bit1 = H short + L long,
 * bit0 = H long + L short) + lone stop high (the 40 ms low after it is the
 * stripped separator). The 32-bit word is a 24-bit payload + 8-bit CRC; the CRC
 * must check, which makes this effectively impossible to false-trigger, so it is
 * tried last. Mirrors the Flipper feed state machine over the segmented frame. */
static bool decode_magellan(const int32_t *d, size_t count,
                            uint64_t *out_data, int *out_bits)
{
    const uint32_t ts = MAGELLAN_TE_SHORT, tl = MAGELLAN_TE_LONG, de = MAGELLAN_DELTA;
    size_t i = 0;
    /* skip a leading header high that is not a short sync pulse (encoder: ts*4);
     * Flipper's Reset syncs on the first short high. */
    while (i < count && !(d[i] > 0 && within((uint32_t)d[i], ts, de))) i++;
    /* >10 (short H, short L) sync pairs, then a (short H, long L) marker. */
    int header = 0;
    bool marker = false;
    while (i + 1 < count) {
        if (d[i] <= 0 || d[i + 1] >= 0) return false;
        uint32_t hi = (uint32_t)d[i], lo = (uint32_t)(-d[i + 1]);
        if (within(hi, ts, de) && within(lo, ts, de)) { header++; i += 2; continue; }
        if (header > MAGELLAN_HEADER_MIN && within(hi, ts, de) && within(lo, tl, de * 2)) {
            i += 2; marker = true; break;
        }
        return false;
    }
    if (!marker) return false;
    /* start bit: (te_short*6 H, long L). */
    if (i + 1 >= count) return false;
    if (d[i] <= 0 || !within((uint32_t)d[i], MAGELLAN_START_HI, de * 3)) return false;
    if (d[i + 1] >= 0 || !within((uint32_t)(-d[i + 1]), tl, de * 2)) return false;
    i += 2;
    /* 32 data bits. */
    uint64_t data = 0;
    int bits = 0;
    while (i + 1 < count && bits < MAGELLAN_BITS) {
        if (d[i] <= 0 || d[i + 1] >= 0) return false;
        uint32_t hi = (uint32_t)d[i], lo = (uint32_t)(-d[i + 1]);
        int bit;
        if (within(hi, ts, de) && within(lo, tl, de)) bit = 1;         /* H short + L long */
        else if (within(hi, tl, de) && within(lo, ts, de)) bit = 0;    /* H long + L short */
        else return false;
        data = (data << 1) | (uint64_t)bit;
        bits++;
        i += 2;
    }
    if (bits != MAGELLAN_BITS) return false;
    /* lone trailing stop high (short), then end of frame. */
    if (i >= count || d[i] <= 0 || !within((uint32_t)d[i], ts, de)) return false;
    if (i + 1 != count) return false;                       /* whole frame consumed */
    /* CRC-8 over the upper three bytes == low byte. */
    uint8_t crc_in[3] = { (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8) };
    if ((uint8_t)(data & 0xFF) != magellan_crc8(crc_in, 3)) return false;
    *out_data = data & 0xFFFFFFFFULL;
    *out_bits = MAGELLAN_BITS;
    return true;
}

/* Intertechno V3: [short H][te_short*10 sync L] then 32 bits, each two (short H,
 * L) pairs — bit1 = (H short, L long)(H short, L short), bit0 = (H short, L short)
 * (H short, L long). The te_short*38 inter-frame low is the (stripped) separator;
 * a trailing short high (the next frame's header) may remain and is tolerated.
 * Gated by the distinctive short-high + 2750 us sync, so it is isolated from the
 * other AM protocols. Only the 32-bit variant (a 36-bit dimming frame is
 * rejected, not mis-decoded). */
static bool decode_intertechno(const int32_t *d, size_t count,
                               uint64_t *out_data, int *out_bits)
{
    const uint32_t ts = INTERTECHNO_TE_SHORT, tl = INTERTECHNO_TE_LONG, de = INTERTECHNO_DELTA;
    if (count < 4) return false;
    if (d[0] <= 0 || !within((uint32_t)d[0], ts, de)) return false;             /* sync high */
    if (d[1] >= 0 || !within((uint32_t)(-d[1]), INTERTECHNO_SYNC_LO, de * 3)) {
        return false;                                                           /* sync low 2750 */
    }
    uint64_t data = 0;
    int bits = 0;
    size_t i = 2;
    while (bits < INTERTECHNO_BITS) {
        /* each bit = [short H][L1][short H] then an optional complement [L2]. The
         * value is set by L1 alone (short=0, long=1); L2 is the complement. The
         * capture trims the FINAL bit's L2 (a trailing low) when no next frame
         * follows, so L2 is required only for non-final bits. */
        if (i + 2 >= count) return false;                   /* need [H][L1][H] */
        if (d[i] <= 0 || !within((uint32_t)d[i], ts, de)) return false;
        if (d[i + 1] >= 0) return false;
        if (d[i + 2] <= 0 || !within((uint32_t)d[i + 2], ts, de)) return false;
        uint32_t l1 = (uint32_t)(-d[i + 1]);
        int bit;
        if (within(l1, tl, de * 2)) bit = 1;                /* long  -> 1 */
        else if (within(l1, ts, de)) bit = 0;               /* short -> 0 */
        else return false;
        data = (data << 1) | (uint64_t)bit;
        bits++;
        if (i + 3 < count && d[i + 3] < 0) {                /* complement low present */
            uint32_t l2 = (uint32_t)(-d[i + 3]);
            if (bit ? !within(l2, ts, de) : !within(l2, tl, de * 2)) return false;
            i += 4;
        } else if (bits == INTERTECHNO_BITS) {
            i += 3;                                         /* final bit: L2 trimmed by capture */
        } else {
            return false;                                   /* non-final bit missing its L2 */
        }
    }
    /* whole-consume; a single trailing short high (next frame's header) is allowed */
    if (i == count || (i + 1 == count && d[i] > 0 && within((uint32_t)d[i], ts, de))) {
        *out_data = data & 0xFFFFFFFFULL;
        *out_bits = INTERTECHNO_BITS;
        return true;
    }
    return false;
}

bool subghz_protocol_decode(const int32_t *durations, size_t count, bool fm,
                            subghz_decoded_t *out)
{
    if (out) { out->proto = SUBGHZ_PROTO_UNKNOWN; out->key = 0; out->bits = 0; out->te = 0; }
    if (!durations || count < 3 || !out) return false;

    size_t filtered_count = 0;
    if (!remove_short_glitches(durations, count, s_decode_durations,
                               &filtered_count)) {
        return false;
    }
    durations = s_decode_durations;
    count = filtered_count;
    if (count < 3) return false;

    uint64_t data = 0;
    int bits = 0;
    uint32_t te = 0;

    if (fm) {
        /* FM/2FSK protocol set. Ansonic's timing overlaps the AM Nice FLO in the
         * jitter band; keeping it out of the AM set (and vice-versa) removes that
         * ambiguity — only the modulation that actually carries each protocol
         * attempts it. */
        if (decode_pwm_frame(durations, count, ANSONIC_TE_SHORT, ANSONIC_DELTA,
                             ANSONIC_TE_SHORT, ANSONIC_TE_LONG,
                             ANSONIC_DELTA, ANSONIC_DELTA, true, &data, &bits) &&
            bits == ANSONIC_BITS) {
            out->proto = SUBGHZ_PROTO_ANSONIC;
            out->key = data; out->bits = ANSONIC_BITS; out->te = ANSONIC_TE_SHORT;
            return true;
        }
        return false;
    }

    /* AM/OOK protocol set. SMC5326 is tried before Princeton: both are 3:1 and
     * Princeton does not whole-consume, so Princeton-first would misread an SMC
     * frame's first 24 pairs. SMC's exact-25 + whole-consume rejects a real
     * Princeton-24 frame, so the order is safe both ways. */
    if (decode_smc5326(durations, count, &data, &bits)) {
        out->proto = SUBGHZ_PROTO_SMC5326;
        out->key = data; out->bits = SMC5326_BITS; out->te = SMC5326_TE_SHORT;
        return true;
    }
    if (decode_princeton(durations, count, &data, &bits, &te)) {
        out->proto = SUBGHZ_PROTO_PRINCETON;
        out->key = data; out->bits = (uint16_t)bits; out->te = te;
        return true;
    }
    if (decode_linear(durations, count, &data, &bits)) {
        out->proto = SUBGHZ_PROTO_LINEAR;
        out->key = data; out->bits = (uint16_t)bits; out->te = LINEAR_TE_SHORT;
        return true;
    }
    if (decode_pwm_high_first(durations, count, CLEMSA_TE_SHORT, CLEMSA_TE_LONG,
                              CLEMSA_D_SHORT, CLEMSA_D_LONG, CLEMSA_END_GAP_MIN,
                              CLEMSA_BITS, &data, &bits)) {
        out->proto = SUBGHZ_PROTO_CLEMSA;
        out->key = data; out->bits = CLEMSA_BITS; out->te = CLEMSA_TE_SHORT;
        return true;
    }
    /* Dooya: gated by its unique start-high (4758 us) + start-low (1466 us)
     * preamble, so it neither steals nor is stolen by the other AM protocols. */
    if (decode_dooya(durations, count, &data, &bits)) {
        out->proto = SUBGHZ_PROTO_DOOYA;
        out->key = data; out->bits = DOOYA_BITS; out->te = DOOYA_TE_SHORT;
        return true;
    }
    /* Intertechno: gated by its short-high + 2750 us sync, isolated from the rest. */
    if (decode_intertechno(durations, count, &data, &bits)) {
        out->proto = SUBGHZ_PROTO_INTERTECHNO;
        out->key = data; out->bits = INTERTECHNO_BITS; out->te = INTERTECHNO_TE_SHORT;
        return true;
    }
    if (decode_pwm_frame(durations, count, HOLTEK_TE_SHORT, HOLTEK_DELTA,
                         HOLTEK_TE_SHORT, HOLTEK_TE_LONG,
                         HOLTEK_DELTA, HOLTEK_DELTA * 2, false, &data, &bits) &&
        bits == HOLTEK_BITS && (data & HOLTEK_HEADER_MASK) == HOLTEK_HEADER) {
        out->proto = SUBGHZ_PROTO_HOLTEK;
        out->key = data; out->bits = HOLTEK_BITS; out->te = HOLTEK_TE_SHORT;
        return true;
    }
    /* Gate-TX before CAME: its start bit is te_long (700), which CAME's start
     * window (320±150) rejects, and vice-versa — the start gate keeps the two
     * 24-bit 2:1 protocols apart. */
    if (decode_pwm_frame(durations, count, GATE_TX_TE_LONG, GATE_TX_START_DELTA,
                         GATE_TX_TE_SHORT, GATE_TX_TE_LONG,
                         GATE_TX_DELTA, GATE_TX_START_DELTA, false, &data, &bits) &&
        bits == GATE_TX_BITS) {
        out->proto = SUBGHZ_PROTO_GATE_TX;
        out->key = data; out->bits = GATE_TX_BITS; out->te = GATE_TX_TE_SHORT;
        return true;
    }
    if (decode_pwm_frame(durations, count, CAME_TE_SHORT, CAME_DELTA,
                         CAME_TE_SHORT, CAME_TE_LONG,
                         CAME_DELTA, CAME_DELTA, false, &data, &bits) &&
        (bits == 12 || bits == 18 || bits == 24 || bits == 25 || bits == 42)) {
        out->proto = SUBGHZ_PROTO_CAME;
        out->key = data; out->bits = (uint16_t)bits; out->te = CAME_TE_SHORT;
        return true;
    }
    if (decode_pwm_frame(durations, count, NICE_TE_SHORT, NICE_DELTA,
                         NICE_TE_SHORT, NICE_TE_LONG,
                         NICE_DELTA, NICE_DELTA, false, &data, &bits) &&
        bits >= 12 && bits <= 24) {
        out->proto = SUBGHZ_PROTO_NICE_FLO;
        out->key = data; out->bits = (uint16_t)bits; out->te = NICE_TE_SHORT;
        return true;
    }
    if (decode_pwm_frame(durations, count, DOITRAND_START_REF, DOITRAND_START_DELTA,
                         DOITRAND_TE_SHORT, DOITRAND_TE_LONG,
                         DOITRAND_D_SHORT, DOITRAND_D_LONG, false, &data, &bits) &&
        bits == DOITRAND_BITS) {
        out->proto = SUBGHZ_PROTO_DOITRAND;
        out->key = data; out->bits = DOITRAND_BITS; out->te = DOITRAND_TE_SHORT;
        return true;
    }
    if (decode_pwm_frame(durations, count, PHOENIX_START, PHOENIX_START_DELTA,
                         PHOENIX_TE_SHORT, PHOENIX_TE_LONG,
                         PHOENIX_D_SHORT, PHOENIX_D_LONG, true, &data, &bits) &&
        bits == PHOENIX_BITS) {
        out->proto = SUBGHZ_PROTO_PHOENIX_V2;
        out->key = data; out->bits = PHOENIX_BITS; out->te = PHOENIX_TE_SHORT;
        return true;
    }
    /* Magellan last: its sync preamble + CRC-8 make it self-validating, so it
     * only claims a frame that nothing else did and that passes the CRC. */
    if (decode_magellan(durations, count, &data, &bits)) {
        out->proto = SUBGHZ_PROTO_MAGELLAN;
        out->key = data; out->bits = MAGELLAN_BITS; out->te = MAGELLAN_TE_SHORT;
        return true;
    }
    return false;
}

/* ---------------------------- encode ---------------------------- */

static size_t encode_princeton(uint32_t te, uint32_t guard, uint64_t key,
                               uint16_t bits, int32_t *out, size_t cap)
{
    if (bits == 0 || bits > 32) return 0;
    if ((size_t)(2 * bits + 2) > cap) return 0;
    size_t n = 0;
    for (int i = (int)bits - 1; i >= 0; i--) {
        if ((key >> i) & 1ULL) {        /* bit 1: H 3Te, L 1Te */
            out[n++] = (int32_t)(te * 3);
            out[n++] = -(int32_t)te;
        } else {                        /* bit 0: H 1Te, L 3Te */
            out[n++] = (int32_t)te;
            out[n++] = -(int32_t)(te * 3);
        }
    }
    out[n++] = (int32_t)te;                    /* stop high */
    out[n++] = -(int32_t)(te * guard);         /* guard low (inter-frame gap) */
    return n;
}

/* PWM 2:1 family: header low (te_short*header_te) + start high + bit pairs
 * (L,H). `start_high` is the start-bit high (te_short for most, te_long for
 * Gate-TX). `inverted` swaps the per-bit polarity (Ansonic). */
static size_t encode_pwm_frame(uint32_t ts, uint32_t tl, uint32_t header_te,
                               uint32_t start_high, bool inverted,
                               uint64_t key, uint16_t bits,
                               int32_t *out, size_t cap)
{
    if (bits == 0 || bits > 64) return 0;   /* key is uint64; Phoenix_V2 = 52 */
    if ((size_t)(2 + 2 * bits) > cap) return 0;
    size_t n = 0;
    out[n++] = -(int32_t)(ts * header_te);     /* header low (gap) */
    out[n++] = (int32_t)start_high;            /* start high */
    for (int i = (int)bits - 1; i >= 0; i--) {
        bool one = ((key >> i) & 1ULL) != 0;
        uint32_t low  = (one != inverted) ? tl : ts;
        uint32_t high = (one != inverted) ? ts : tl;
        out[n++] = -(int32_t)low;
        out[n++] = (int32_t)high;
    }
    return n;
}

/* CAME/Nice/Holtek: start high = te_short, non-inverted. */
static size_t encode_came_like(uint32_t ts, uint32_t tl, uint32_t header_te,
                               uint64_t key, uint16_t bits,
                               int32_t *out, size_t cap)
{
    return encode_pwm_frame(ts, tl, header_te, ts, false, key, bits, out, cap);
}

/* HIGH-first PWM (Linear, Clemsa): H-then-L pairs, no header/start. bit1 = H
 * long + L short, bit0 = H short + L long; the LAST bit's low leg is the
 * inter-frame gap (trailer_lo_1 for a final 1, trailer_lo_0 for a final 0). */
static size_t encode_pwm_high_first(uint32_t ts, uint32_t tl, uint64_t key,
                                    uint16_t bits, uint32_t trailer_lo_1,
                                    uint32_t trailer_lo_0, int32_t *out, size_t cap)
{
    if (bits == 0 || bits > 32) return 0;
    if ((size_t)(2 * bits) > cap) return 0;
    size_t n = 0;
    for (int i = (int)bits - 1; i >= 1; i--) {
        if ((key >> i) & 1ULL) {               /* bit 1: H long, L short */
            out[n++] = (int32_t)tl;
            out[n++] = -(int32_t)ts;
        } else {                               /* bit 0: H short, L long */
            out[n++] = (int32_t)ts;
            out[n++] = -(int32_t)tl;
        }
    }
    if (key & 1ULL) {                          /* last bit 1 + inter-frame gap */
        out[n++] = (int32_t)tl;
        out[n++] = -(int32_t)trailer_lo_1;
    } else {                                   /* last bit 0 + inter-frame gap */
        out[n++] = (int32_t)ts;
        out[n++] = -(int32_t)trailer_lo_0;
    }
    return n;
}

static size_t encode_linear(uint32_t ts, uint32_t tl, uint64_t key,
                            uint16_t bits, int32_t *out, size_t cap)
{
    return encode_pwm_high_first(ts, tl, key, bits,
                                 ts * LINEAR_END_GAP_TE1, ts * LINEAR_END_GAP_TE0,
                                 out, cap);
}

/* Dooya: header low (te_long*12 + te_long|te_short on the LSB) + start high
 * (te_short*13) + start low (te_long*2) + `bits` HIGH-first 2:1 pairs (bit1 = H
 * long + L short, bit0 = H short + L long). Mirrors Flipper's get_upload; the
 * leading header low is the inter-frame separator for repeats. */
static size_t encode_dooya(uint64_t key, uint16_t bits, int32_t *out, size_t cap)
{
    if (bits == 0 || bits > 40) return 0;
    if ((size_t)(3 + 2 * bits) > cap) return 0;
    size_t n = 0;
    uint32_t header_lo = (uint32_t)DOOYA_TE_LONG * DOOYA_HEADER_TE +
                         ((key & 1ULL) ? DOOYA_TE_LONG : DOOYA_TE_SHORT);
    out[n++] = -(int32_t)header_lo;             /* inter-frame header low (~9 ms) */
    out[n++] = (int32_t)DOOYA_PREAMBLE_HI;      /* start high te_short*13 */
    out[n++] = -(int32_t)DOOYA_PREAMBLE_LO;     /* start low te_long*2 */
    for (int i = (int)bits - 1; i >= 0; i--) {
        if ((key >> i) & 1ULL) {                /* bit 1: H long, L short */
            out[n++] = (int32_t)DOOYA_TE_LONG;
            out[n++] = -(int32_t)DOOYA_TE_SHORT;
        } else {                                /* bit 0: H short, L long */
            out[n++] = (int32_t)DOOYA_TE_SHORT;
            out[n++] = -(int32_t)DOOYA_TE_LONG;
        }
    }
    return n;
}

/* Magellan: header (ts*4, ts) + 12 (short,short) sync pairs + (short,long) marker
 * + (ts*6, long) start bit + `bits` HIGH-first 2:1 pairs (bit1 = H short + L long,
 * bit0 = H long + L short) + stop high + te_long*100 separator. Mirrors Flipper's
 * get_upload; the CRC is already part of `key` (low byte). */
static size_t encode_magellan(uint64_t key, uint16_t bits, int32_t *out, size_t cap)
{
    const uint32_t ts = MAGELLAN_TE_SHORT, tl = MAGELLAN_TE_LONG;
    if (bits != MAGELLAN_BITS) return 0;
    if ((size_t)(2 + 24 + 2 + 2 + 2 * bits + 2) > cap) return 0;
    size_t n = 0;
    out[n++] = (int32_t)(ts * 4); out[n++] = -(int32_t)ts;      /* header */
    for (int k = 0; k < 12; k++) {                              /* 12 sync pairs */
        out[n++] = (int32_t)ts; out[n++] = -(int32_t)ts;
    }
    out[n++] = (int32_t)ts; out[n++] = -(int32_t)tl;            /* marker (short, long) */
    out[n++] = (int32_t)(ts * 6); out[n++] = -(int32_t)tl;      /* start bit */
    for (int i = (int)bits - 1; i >= 0; i--) {
        if ((key >> i) & 1ULL) {                                /* bit 1: H short, L long */
            out[n++] = (int32_t)ts; out[n++] = -(int32_t)tl;
        } else {                                                /* bit 0: H long, L short */
            out[n++] = (int32_t)tl; out[n++] = -(int32_t)ts;
        }
    }
    out[n++] = (int32_t)ts; out[n++] = -(int32_t)(tl * 100);    /* stop + inter-frame sep */
    return n;
}

/* Intertechno V3 (32-bit): short-high header + te_short*38 separator low +
 * short-high + te_short*10 sync low + 32 bits, each two (short H, L) pairs
 * (bit1 = (ts,tl)(ts,ts), bit0 = (ts,ts)(ts,tl)). Mirrors Flipper's get_upload. */
static size_t encode_intertechno(uint64_t key, uint16_t bits, int32_t *out, size_t cap)
{
    const uint32_t ts = INTERTECHNO_TE_SHORT, tl = INTERTECHNO_TE_LONG;
    if (bits != INTERTECHNO_BITS) return 0;
    if ((size_t)(4 + 4 * bits) > cap) return 0;
    size_t n = 0;
    out[n++] = (int32_t)ts; out[n++] = -(int32_t)(ts * INTERTECHNO_SEP_TE);  /* header + separator */
    out[n++] = (int32_t)ts; out[n++] = -(int32_t)INTERTECHNO_SYNC_LO;        /* sync high + 2750 low */
    for (int i = (int)bits - 1; i >= 0; i--) {
        if ((key >> i) & 1ULL) {                   /* bit 1: (ts, tl)(ts, ts) */
            out[n++] = (int32_t)ts; out[n++] = -(int32_t)tl;
            out[n++] = (int32_t)ts; out[n++] = -(int32_t)ts;
        } else {                                   /* bit 0: (ts, ts)(ts, tl) */
            out[n++] = (int32_t)ts; out[n++] = -(int32_t)ts;
            out[n++] = (int32_t)ts; out[n++] = -(int32_t)tl;
        }
    }
    return n;
}

size_t subghz_protocol_encode(subghz_proto_id_t proto, uint64_t key,
                              uint16_t bits, uint32_t te,
                              int32_t *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    switch (proto) {
        case SUBGHZ_PROTO_PRINCETON:
            return encode_princeton(te ? te : PRINCETON_TE_SHORT, PRINCETON_GUARD,
                                    key, bits ? bits : PRINCETON_BITS, out, cap);
        case SUBGHZ_PROTO_LINEAR:
            return encode_linear(LINEAR_TE_SHORT, LINEAR_TE_LONG, key,
                                 bits ? bits : LINEAR_BITS, out, cap);
        case SUBGHZ_PROTO_CAME: {
            if (bits == 0) bits = 12;
            uint32_t h = (bits == 24 || bits == 42) ? 76
                       : (bits == 12 || bits == 18) ? 47
                       : (bits == 25)               ? 36
                                                    : 16;
            return encode_came_like(CAME_TE_SHORT, CAME_TE_LONG, h, key, bits, out, cap);
        }
        case SUBGHZ_PROTO_NICE_FLO:
            return encode_came_like(NICE_TE_SHORT, NICE_TE_LONG, 36, key,
                                    bits ? bits : 12, out, cap);
        case SUBGHZ_PROTO_HOLTEK:
            return encode_came_like(HOLTEK_TE_SHORT, HOLTEK_TE_LONG, 36, key,
                                    bits ? bits : HOLTEK_BITS, out, cap);
        case SUBGHZ_PROTO_GATE_TX:
            return encode_pwm_frame(GATE_TX_TE_SHORT, GATE_TX_TE_LONG,
                                    GATE_TX_HEADER_TE, GATE_TX_TE_LONG /*start*/,
                                    false, key, bits ? bits : GATE_TX_BITS, out, cap);
        case SUBGHZ_PROTO_ANSONIC:
            return encode_pwm_frame(ANSONIC_TE_SHORT, ANSONIC_TE_LONG,
                                    ANSONIC_HEADER_TE, ANSONIC_TE_SHORT /*start*/,
                                    true, key, bits ? bits : ANSONIC_BITS, out, cap);
        case SUBGHZ_PROTO_DOITRAND:
            return encode_pwm_frame(DOITRAND_TE_SHORT, DOITRAND_TE_LONG,
                                    DOITRAND_HEADER_TE, DOITRAND_START /*start*/,
                                    false, key, bits ? bits : DOITRAND_BITS, out, cap);
        case SUBGHZ_PROTO_PHOENIX_V2:
            return encode_pwm_frame(PHOENIX_TE_SHORT, PHOENIX_TE_LONG,
                                    PHOENIX_HEADER_TE, PHOENIX_START /*start*/,
                                    true, key, bits ? bits : PHOENIX_BITS, out, cap);
        case SUBGHZ_PROTO_CLEMSA:
            return encode_pwm_high_first(CLEMSA_TE_SHORT, CLEMSA_TE_LONG, key,
                                         bits ? bits : CLEMSA_BITS,
                                         CLEMSA_TRAILER1, CLEMSA_TRAILER0, out, cap);
        case SUBGHZ_PROTO_SMC5326:
            /* identical frame shape to Princeton (3:1 pairs + stop + te*guard),
             * only the guard differs (25 vs 30) */
            return encode_princeton(te ? te : SMC5326_TE_SHORT, SMC5326_GUARD,
                                    key, bits ? bits : SMC5326_BITS, out, cap);
        case SUBGHZ_PROTO_DOOYA:
            return encode_dooya(key, bits ? bits : DOOYA_BITS, out, cap);
        case SUBGHZ_PROTO_MAGELLAN:
            return encode_magellan(key, bits ? bits : MAGELLAN_BITS, out, cap);
        case SUBGHZ_PROTO_INTERTECHNO:
            return encode_intertechno(key, bits ? bits : INTERTECHNO_BITS, out, cap);
        default:
            return 0;
    }
}
