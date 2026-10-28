/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ir_proto.h"

#include <string.h>
#include <strings.h>

/* ============================================================
 * Timing constants (microseconds)
 * ============================================================ */
#define NEC_HDR_MARK    9000
#define NEC_HDR_SPACE   4500
#define NEC_BIT_MARK     560
#define NEC_ONE_SPACE   1690
#define NEC_ZERO_SPACE   560

#define SAM_HDR_MARK    4500
#define SAM_HDR_SPACE   4500
#define SAM_BIT_MARK     550
#define SAM_ONE_SPACE   1650
#define SAM_ZERO_SPACE   550

#define PIONEER_HDR_MARK   8500
#define PIONEER_HDR_SPACE  4225
#define PIONEER_BIT_MARK    500
#define PIONEER_ONE_SPACE  1500
#define PIONEER_ZERO_SPACE  500

#define SONY_HDR_MARK   2400
#define SONY_HDR_SPACE   600
#define SONY_ONE_MARK   1200
#define SONY_ZERO_MARK   600
#define SONY_SPACE       600

#define RCA_HDR_MARK    4000
#define RCA_HDR_SPACE   4000
#define RCA_BIT_MARK     500
#define RCA_ONE_SPACE   2000
#define RCA_ZERO_SPACE  1000

#define RC5_UNIT         888   /* half-bit */
#define RC6_UNIT         444

/* ============================================================
 * Names
 * ============================================================ */
static const char *const k_names[IR_PROTO_COUNT] = {
    [IR_PROTO_UNKNOWN]   = "Unknown",
    [IR_PROTO_NEC]       = "NEC",
    [IR_PROTO_NECEXT]    = "NECext",
    [IR_PROTO_NEC42]     = "NEC42",
    [IR_PROTO_SAMSUNG32] = "Samsung32",
    [IR_PROTO_SONY]      = "SIRC",
    [IR_PROTO_RC5]       = "RC5",
    [IR_PROTO_RC6]       = "RC6",
    [IR_PROTO_RCA]       = "RCA",
    [IR_PROTO_PIONEER]   = "Pioneer",
    [IR_PROTO_KASEIKYO]  = "Kaseikyo",
    [IR_PROTO_RAW]       = "RAW",
};

const char *ir_proto_name(ir_proto_t p)
{
    if (p < 0 || p >= IR_PROTO_COUNT) return "Unknown";
    return k_names[p] ? k_names[p] : "Unknown";
}

ir_proto_t ir_proto_from_name(const char *s)
{
    if (!s) return IR_PROTO_UNKNOWN;
    for (int i = 0; i < IR_PROTO_COUNT; i++) {
        if (k_names[i] && strcasecmp(s, k_names[i]) == 0) return (ir_proto_t)i;
    }
    /* Accept a few aliases. */
    if (strcasecmp(s, "sony") == 0)      return IR_PROTO_SONY;
    if (strcasecmp(s, "samsung") == 0)   return IR_PROTO_SAMSUNG32;
    if (strcasecmp(s, "panasonic") == 0) return IR_PROTO_KASEIKYO;
    return IR_PROTO_UNKNOWN;
}

int ir_proto_min_repeats(ir_proto_t p)
{
    if (p == IR_PROTO_SONY) return 3;
    if (p == IR_PROTO_PIONEER) return 2;
    return 1;
}

/* ============================================================
 * Timing-list builder
 * ============================================================ */
typedef struct {
    uint16_t *buf;
    size_t    cap;
    size_t    n;
    bool      ok;
} tbuf_t;

static void tb_push(tbuf_t *t, uint32_t us)
{
    if (!t->ok) return;
    if (t->n >= t->cap) { t->ok = false; return; }
    if (us > 0xFFFF) us = 0xFFFF;
    t->buf[t->n++] = (uint16_t)us;
}

/* Pulse-distance frame (NEC/Samsung/RCA style). bits are sent in the
 * caller-provided order (already arranged LSB/MSB first). */
static void emit_pulse_distance(tbuf_t *t,
                                uint16_t hdr_mark, uint16_t hdr_space,
                                uint16_t bit_mark,
                                uint16_t one_space, uint16_t zero_space,
                                const uint8_t *bits, size_t nbits,
                                uint16_t trail_mark)
{
    tb_push(t, hdr_mark);
    tb_push(t, hdr_space);
    for (size_t i = 0; i < nbits; i++) {
        tb_push(t, bit_mark);
        tb_push(t, bits[i] ? one_space : zero_space);
    }
    tb_push(t, trail_mark);
}

/* Manchester / bi-phase: caller supplies a stream of per-unit logic
 * samples (1 = carrier ON, 0 = OFF). Consecutive equal samples are
 * run-length merged into mark/space durations. Leading and trailing
 * OFF runs (idle) are dropped so the result starts and ends with a
 * mark. */
static void emit_unit_samples(tbuf_t *t, const uint8_t *samples,
                              size_t nsamp, uint16_t unit)
{
    /* Skip leading idle. */
    size_t start = 0;
    while (start < nsamp && samples[start] == 0) start++;
    /* Skip trailing idle. */
    size_t end = nsamp;
    while (end > start && samples[end - 1] == 0) end--;
    if (start >= end) return;

    size_t i = start;
    while (i < end) {
        uint8_t level = samples[i];
        size_t run = 0;
        while (i < end && samples[i] == level) { run++; i++; }
        tb_push(t, (uint32_t)run * unit);
    }
}

/* ============================================================
 * Per-protocol encoders
 * ============================================================ */

/* Append `count` LSB-first bits of `value` to a bit array. */
static size_t bits_lsb(uint8_t *dst, uint64_t value, int count)
{
    for (int i = 0; i < count; i++) dst[i] = (value >> i) & 1u;
    return (size_t)count;
}
static size_t bits_msb(uint8_t *dst, uint64_t value, int count)
{
    for (int i = 0; i < count; i++) dst[i] = (value >> (count - 1 - i)) & 1u;
    return (size_t)count;
}

static size_t enc_nec(const ir_message_t *m, tbuf_t *t, bool ext)
{
    uint8_t bits[32];
    size_t n = 0;
    if (ext) {
        /* NECext: 16-bit address + 16-bit command, both sent verbatim
         * (the command field already carries its complement byte when the
         * remote uses one — Flipper stores the full 16-bit value). */
        n += bits_lsb(bits + n, (uint16_t)m->address, 16);
        n += bits_lsb(bits + n, (uint16_t)m->command, 16);
    } else {
        /* NEC: 8-bit address + command, encoder appends the complements. */
        uint8_t a = (uint8_t)m->address, na = (uint8_t)~a;
        uint8_t c = (uint8_t)m->command, nc = (uint8_t)~c;
        n += bits_lsb(bits + n, a, 8);
        n += bits_lsb(bits + n, na, 8);
        n += bits_lsb(bits + n, c, 8);
        n += bits_lsb(bits + n, nc, 8);
    }
    emit_pulse_distance(t, NEC_HDR_MARK, NEC_HDR_SPACE, NEC_BIT_MARK,
                        NEC_ONE_SPACE, NEC_ZERO_SPACE, bits, n, NEC_BIT_MARK);
    return t->ok ? t->n : 0;
}

/* Pioneer is NEC-like, but uses its own timing and a 33rd trailing data bit. */
static size_t enc_pioneer(const ir_message_t *m, tbuf_t *t)
{
    uint8_t bits[33];
    const uint8_t address = (uint8_t)m->address;
    const uint8_t command = (uint8_t)m->command;
    size_t n = 0;

    n += bits_lsb(bits + n, address, 8);
    n += bits_lsb(bits + n, (uint8_t)~address, 8);
    n += bits_lsb(bits + n, command, 8);
    n += bits_lsb(bits + n, (uint8_t)~command, 8);
    bits[n++] = 0;

    /* Upstream Pioneer ends after the last bit's space, then emits its
     * 26 ms silence as the inter-frame repeat gap. There is no extra
     * trailer mark after that space. */
    tb_push(t, PIONEER_HDR_MARK);
    tb_push(t, PIONEER_HDR_SPACE);
    for (size_t i = 0; i < n; i++) {
        tb_push(t, PIONEER_BIT_MARK);
        tb_push(t, bits[i] ? PIONEER_ONE_SPACE : PIONEER_ZERO_SPACE);
    }
    return t->ok ? t->n : 0;
}

/* NEC42: 13-bit address + 13-bit ~address + 8-bit command + 8-bit ~command. */
static size_t enc_nec42(const ir_message_t *m, tbuf_t *t)
{
    uint8_t bits[42];
    uint16_t a = (uint16_t)(m->address & 0x1FFF);
    uint16_t na = (uint16_t)((~a) & 0x1FFF);
    uint8_t c = (uint8_t)m->command, nc = (uint8_t)~c;
    size_t n = 0;
    n += bits_lsb(bits + n, a, 13);
    n += bits_lsb(bits + n, na, 13);
    n += bits_lsb(bits + n, c, 8);
    n += bits_lsb(bits + n, nc, 8);
    emit_pulse_distance(t, NEC_HDR_MARK, NEC_HDR_SPACE, NEC_BIT_MARK,
                        NEC_ONE_SPACE, NEC_ZERO_SPACE, bits, n, NEC_BIT_MARK);
    return t->ok ? t->n : 0;
}

static size_t enc_samsung(const ir_message_t *m, tbuf_t *t)
{
    uint8_t bits[32];
    uint8_t a = (uint8_t)m->address;
    uint8_t c = (uint8_t)m->command, nc = (uint8_t)~c;
    size_t n = 0;
    n += bits_lsb(bits + n, a, 8);
    n += bits_lsb(bits + n, a, 8);
    n += bits_lsb(bits + n, c, 8);
    n += bits_lsb(bits + n, nc, 8);
    emit_pulse_distance(t, SAM_HDR_MARK, SAM_HDR_SPACE, SAM_BIT_MARK,
                        SAM_ONE_SPACE, SAM_ZERO_SPACE, bits, n, SAM_BIT_MARK);
    return t->ok ? t->n : 0;
}

static size_t enc_rca(const ir_message_t *m, tbuf_t *t)
{
    uint8_t bits[24];
    uint8_t a = (uint8_t)(m->address & 0x0F);
    uint8_t c = (uint8_t)m->command;
    uint8_t na = (uint8_t)(~a & 0x0F);
    uint8_t nc = (uint8_t)~c;
    size_t n = 0;
    n += bits_msb(bits + n, a, 4);
    n += bits_msb(bits + n, c, 8);
    n += bits_msb(bits + n, na, 4);
    n += bits_msb(bits + n, nc, 8);
    emit_pulse_distance(t, RCA_HDR_MARK, RCA_HDR_SPACE, RCA_BIT_MARK,
                        RCA_ONE_SPACE, RCA_ZERO_SPACE, bits, n, RCA_BIT_MARK);
    return t->ok ? t->n : 0;
}

static size_t enc_sony(const ir_message_t *m, tbuf_t *t)
{
    int nbits = m->bits ? m->bits : 12;
    if (nbits != 12 && nbits != 15 && nbits != 20) nbits = 12;
    uint64_t v = (m->command & 0x7F) | ((uint64_t)m->address << 7);

    tb_push(t, SONY_HDR_MARK);
    tb_push(t, SONY_HDR_SPACE);
    for (int i = 0; i < nbits; i++) {
        int bit = (v >> i) & 1u;
        tb_push(t, bit ? SONY_ONE_MARK : SONY_ZERO_MARK);
        tb_push(t, SONY_SPACE);
    }
    return t->ok ? t->n : 0;
}

/* RC5: 14 bits MSB-first — S1,S2,toggle, 5 addr, 6 cmd.
 * bi-phase: '1' = low,high ; '0' = high,low (half = RC5_UNIT). */
static size_t enc_rc5(const ir_message_t *m, tbuf_t *t)
{
    uint8_t logic[14];
    size_t n = 0;
    logic[n++] = 1;                             /* S1 */
    logic[n++] = ((m->command >> 6) & 1) ? 0 : 1; /* S2 = ~cmd bit6 (RC5X) */
    logic[n++] = m->toggle ? 1 : 0;             /* toggle */
    n += bits_msb(logic + n, m->address & 0x1F, 5);
    n += bits_msb(logic + n, m->command & 0x3F, 6);

    uint8_t samples[28];
    size_t s = 0;
    for (size_t i = 0; i < n; i++) {
        if (logic[i]) { samples[s++] = 0; samples[s++] = 1; }
        else          { samples[s++] = 1; samples[s++] = 0; }
    }
    emit_unit_samples(t, samples, s, RC5_UNIT);
    return t->ok ? t->n : 0;
}

/* RC6 mode 0: leader + start + 3 mode bits (0) + doubled toggle + 16 data.
 * bi-phase: '1' = high,low ; '0' = low,high (half = RC6_UNIT). */
static size_t enc_rc6(const ir_message_t *m, tbuf_t *t)
{
    uint8_t samples[200];
    size_t s = 0;
    /* Use Unleashed's measured leader values (2666/889 us), then encode
     * the start/mode/toggle/data half-bits at 444 us. */
    tb_push(t, 2666);
    tb_push(t, 889);
    /* Start bit '1' = high,low. */
    samples[s++] = 1; samples[s++] = 0;
    /* Mode = 000, each '0' = low,high. */
    for (int i = 0; i < 3; i++) { samples[s++] = 0; samples[s++] = 1; }
    /* Toggle half-bit is doubled. */
    if (m->toggle) {
        samples[s++] = 1; samples[s++] = 1; samples[s++] = 0; samples[s++] = 0;
    } else {
        samples[s++] = 0; samples[s++] = 0; samples[s++] = 1; samples[s++] = 1;
    }
    /* 16 data bits MSB first: 8 control (address) + 8 info (command). */
    uint16_t data = (uint16_t)(((m->address & 0xFF) << 8) | (m->command & 0xFF));
    for (int i = 15; i >= 0; i--) {
        int bit = (data >> i) & 1u;
        if (bit) { samples[s++] = 1; samples[s++] = 0; }
        else     { samples[s++] = 0; samples[s++] = 1; }
    }
    emit_unit_samples(t, samples, s, RC6_UNIT);
    return t->ok ? t->n : 0;
}

/* Kaseikyo (Panasonic family): 48-bit, LSB-first, unit 432 us.
 * Frame bytes (matches Flipper's decoder):
 *   d0,d1 = vendor id (16)   d2 = vendor_parity(4) | genre1(4)<<4
 *   d3 = genre2(4) | cmd[0:4]<<4   d4 = cmd[4:10] | id(2)<<6
 *   d5 = d2 ^ d3 ^ d4 (checksum)
 * address packs id<<24 | vendor<<8 | genre1<<4 | genre2. */
static size_t enc_kaseikyo(const ir_message_t *m, tbuf_t *t)
{
    uint32_t addr = m->address;
    uint16_t cmd = (uint16_t)(m->command & 0x3FF);
    uint8_t  id = (addr >> 24) & 0x3;
    uint16_t vendor = (addr >> 8) & 0xFFFF;
    uint8_t  genre1 = (addr >> 4) & 0xF;
    uint8_t  genre2 = addr & 0xF;

    uint8_t d[6];
    d[0] = vendor & 0xFF;
    d[1] = (vendor >> 8) & 0xFF;
    uint8_t p8 = d[0] ^ d[1];
    uint8_t vparity = (p8 & 0xF) ^ (p8 >> 4);
    d[2] = (uint8_t)((vparity & 0xF) | (genre1 << 4));
    d[3] = (uint8_t)((genre2 & 0xF) | ((cmd & 0xF) << 4));
    d[4] = (uint8_t)(((cmd >> 4) & 0x3F) | (id << 6));
    d[5] = d[2] ^ d[3] ^ d[4];

    uint8_t bits[48];
    size_t n = 0;
    for (int byte = 0; byte < 6; byte++)
        for (int b = 0; b < 8; b++) bits[n++] = (d[byte] >> b) & 1;
    emit_pulse_distance(t, 3456, 1728, 432, 1296, 432, bits, 48, 432);
    return t->ok ? t->n : 0;
}

size_t ir_encode(const ir_message_t *msg, uint16_t *out, size_t out_cap,
                 uint32_t *carrier_hz_out, float *duty_out)
{
    if (!msg || !out || out_cap == 0) return 0;
    tbuf_t t = { .buf = out, .cap = out_cap, .n = 0, .ok = true };
    uint32_t carrier = 38000;
    float duty = 0.33f;
    size_t n = 0;

    switch (msg->proto) {
    case IR_PROTO_NEC:       n = enc_nec(msg, &t, false); break;
    case IR_PROTO_NECEXT:    n = enc_nec(msg, &t, true);  break;
    case IR_PROTO_NEC42:     n = enc_nec42(msg, &t);      break;
    case IR_PROTO_SAMSUNG32: n = enc_samsung(msg, &t);    break;
    case IR_PROTO_RCA:       n = enc_rca(msg, &t); carrier = 56000; break;
    case IR_PROTO_SONY:      n = enc_sony(msg, &t); carrier = 40000; break;
    case IR_PROTO_PIONEER:   n = enc_pioneer(msg, &t); carrier = 40000; break;
    case IR_PROTO_RC5:       n = enc_rc5(msg, &t); carrier = 36000; break;
    case IR_PROTO_RC6:       n = enc_rc6(msg, &t); carrier = 36000; break;
    case IR_PROTO_KASEIKYO:  n = enc_kaseikyo(msg, &t); carrier = 37000; break;
    default:                 n = 0; break;  /* unknown/raw: encode unsupported */
    }

    if (!t.ok || n == 0) return 0;
    if (carrier_hz_out) *carrier_hz_out = carrier;
    if (duty_out)       *duty_out = duty;
    return n;
}

/* ============================================================
 * Decode (best-effort)
 * ============================================================ */
static bool tmatch(uint16_t v, uint16_t target)
{
    int d = (int)v - (int)target;
    int tol = (int)target * 30 / 100 + 150;
    return d >= -tol && d <= tol;
}

/* Decode a pulse-distance frame: read `want` bits as (mark, space) pairs
 * starting at timings[idx]; a space longer than `thresh` is a '1'. The
 * value is assembled LSB-first. Returns true if enough pairs existed. */
static bool read_pd_lsb(const uint16_t *t, size_t count, size_t idx,
                        int want, uint16_t thresh, uint64_t *out)
{
    uint64_t v = 0;
    for (int k = 0; k < want; k++) {
        size_t mi = idx + (size_t)k * 2;
        size_t si = mi + 1;
        if (si >= count) return false;
        int bit = (t[si] > thresh) ? 1 : 0;
        v |= ((uint64_t)bit) << k;
    }
    *out = v;
    return true;
}

static bool read_pd_msb(const uint16_t *t, size_t count, size_t idx,
                        int want, uint16_t thresh, uint64_t *out)
{
    uint64_t v = 0;
    for (int k = 0; k < want; k++) {
        size_t mi = idx + (size_t)k * 2;
        size_t si = mi + 1;
        if (si >= count) return false;
        int bit = (t[si] > thresh) ? 1 : 0;
        v = (v << 1) | (uint64_t)bit;
    }
    *out = v;
    return true;
}

static bool decode_nec_family(const uint16_t *t, size_t count, ir_message_t *out)
{
    if (count < 6) return false;
    bool nec_hdr = tmatch(t[0], NEC_HDR_MARK) && tmatch(t[1], NEC_HDR_SPACE);
    bool sam_hdr = tmatch(t[0], SAM_HDR_MARK) && tmatch(t[1], SAM_HDR_SPACE);
    if (!nec_hdr && !sam_hdr) return false;

    uint64_t v = 0;
    if (!read_pd_lsb(t, count, 2, 32, 1000, &v)) return false;
    uint8_t b0 = v & 0xFF, b1 = (v >> 8) & 0xFF;
    uint8_t b2 = (v >> 16) & 0xFF, b3 = (v >> 24) & 0xFF;

    if (sam_hdr) {
        out->proto = IR_PROTO_SAMSUNG32;
        out->address = b0;
        out->command = b2;
        out->bits = 32;
        return true;
    }
    /* NEC */
    if (((b0 ^ b1) == 0xFF) && ((b2 ^ b3) == 0xFF)) {
        out->proto = IR_PROTO_NEC;
        out->address = b0;
        out->command = b2;
        out->bits = 32;
    } else {
        out->proto = IR_PROTO_NECEXT;
        out->address = (uint16_t)(b0 | (b1 << 8));
        out->command = b2;
        out->bits = 32;
    }
    return true;
}

static bool decode_rca(const uint16_t *t, size_t count, ir_message_t *out)
{
    if (count < 6) return false;
    if (!(tmatch(t[0], RCA_HDR_MARK) && tmatch(t[1], RCA_HDR_SPACE))) return false;
    uint64_t v = 0;
    if (!read_pd_msb(t, count, 2, 24, 1500, &v)) return false;
    uint8_t addr = (v >> 20) & 0x0F, cmd = (v >> 12) & 0xFF;
    uint8_t naddr = (v >> 8) & 0x0F, ncmd = v & 0xFF;
    /* RCA repeats each field inverted; verify to avoid false positives. */
    if (((addr ^ naddr) & 0x0F) != 0x0F || (uint8_t)(cmd ^ ncmd) != 0xFF) {
        return false;
    }
    out->proto = IR_PROTO_RCA;
    out->address = addr;
    out->command = cmd;
    out->bits = 24;
    return true;
}

static bool decode_sony(const uint16_t *t, size_t count, ir_message_t *out)
{
    if (count < 4) return false;
    if (!(tmatch(t[0], SONY_HDR_MARK) && tmatch(t[1], SONY_HDR_SPACE))) return false;

    /* Count bit pairs (mark,space) after the header. */
    int nbits = 0;
    uint64_t v = 0;
    for (size_t idx = 2; idx + 1 <= count; idx += 2) {
        if (idx + 1 > count) break;
        uint16_t mark = t[idx];
        /* A valid SIRC mark is ~600 or ~1200; stop at the first that isn't. */
        if (!tmatch(mark, SONY_ZERO_MARK) && !tmatch(mark, SONY_ONE_MARK)) break;
        int bit = tmatch(mark, SONY_ONE_MARK) ? 1 : 0;
        v |= ((uint64_t)bit) << nbits;
        nbits++;
        if (nbits >= 20) break;
    }
    if (nbits != 12 && nbits != 15 && nbits != 20) return false;
    out->proto = IR_PROTO_SONY;
    out->command = v & 0x7F;
    out->address = (uint32_t)(v >> 7);
    out->bits = (uint8_t)nbits;
    return true;
}

bool ir_decode(const uint16_t *timings, size_t count, ir_message_t *out)
{
    if (!timings || count < 4 || !out) {
        if (out) { memset(out, 0, sizeof(*out)); out->proto = IR_PROTO_UNKNOWN; }
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->proto = IR_PROTO_UNKNOWN;

    if (decode_nec_family(timings, count, out)) return true;
    if (decode_sony(timings, count, out))       return true;
    if (decode_rca(timings, count, out))        return true;

    out->proto = IR_PROTO_UNKNOWN;
    return false;
}
