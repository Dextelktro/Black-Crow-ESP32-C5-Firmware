#include "subghz_protocol.h"

#include <strings.h>

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
    size_t i = 0;
    while (i + 1 < count && bits < PRINCETON_BITS) {
        int32_t high = d[i], low = d[i + 1];
        if (high <= 0 || low >= 0) break;
        uint32_t ha = (uint32_t)high, la = (uint32_t)(-low);
        uint32_t mn = ha < la ? ha : la;
        uint32_t mx = ha < la ? la : ha;
        /* Discriminate by the 3:1 RATIO, not loose absolute windows: without
         * Flipper's preamble as a gate, absolute windows would let a 2:1
         * protocol (CAME/Nice/Holtek) satisfy 24 Princeton bits and be
         * misclassified. Require the long leg ~2.33x..4x the short (3:1), and a
         * plausible short-pulse Te. */
        if (mn < 80 || mn > 2000) break;
        if (mx < mn * 2 + mn / 3 || mx > mn * 4) break;
        int bit = (ha > la) ? 1 : 0;   /* long-high = 1, short-high = 0 */
        data = (data << 1) | (uint64_t)bit;
        total += ha + la;
        bits++;
        i += 2;
    }
    if (bits != PRINCETON_BITS) return false;
    *out_data = data & 0xFFFFFFULL;
    *out_bits = PRINCETON_BITS;
    *out_te = (uint32_t)(total / (PRINCETON_BITS * 4));   /* each bit = 4 Te */
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

bool subghz_protocol_decode(const int32_t *durations, size_t count, bool fm,
                            subghz_decoded_t *out)
{
    if (out) { out->proto = SUBGHZ_PROTO_UNKNOWN; out->key = 0; out->bits = 0; out->te = 0; }
    if (!durations || count < 3 || !out) return false;

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

    /* AM/OOK protocol set. */
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
        default:
            return 0;
    }
}
