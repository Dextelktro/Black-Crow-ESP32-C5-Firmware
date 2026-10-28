/*
 * SPDX-FileCopyrightText: 2026 Black Crow Official Community Dxt and contributors
 * SPDX-FileCopyrightText: Flipper Devices Inc. and the flipperzero-firmware contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Contains material derived from the GPL-3.0 Flipper Zero firmware; see NOTICE.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * Static fixed-code OOK Sub-GHz protocols (decode + encode), ported faithfully
 * from Flipper Zero Sub-GHz protocol sources: timing constants, bit order and
 * frame structure match so captures/keys interoperate.
 *
 * A captured burst arrives already segmented: the inter-frame gap of these
 * protocols (11-25 ms) exceeds the RMT capture stop threshold (10 ms), so each
 * frame is one burst — leading gap trimmed, starting on a mark. Decoders here
 * therefore operate on a whole single frame (simpler than Flipper's streaming
 * state machine) using the same tolerance windows.
 *
 * Encoders build a (signed-us) timing list for one frame, Flipper RAW
 * convention (+ = carrier/high, - = off/low), ready for the GPTimer TX path.
 * Used by Read (decode/label) and Brute Force (encode a code space).
 * ============================================================ */

typedef enum {
    SUBGHZ_PROTO_UNKNOWN = 0,
    SUBGHZ_PROTO_PRINCETON,   /* PT2262-style, 24-bit, 3:1 */
    SUBGHZ_PROTO_LINEAR,      /* 10-bit, 3:1 (H,L), no start bit */
    SUBGHZ_PROTO_CAME,        /* 12/18/24/25/42-bit, 2:1 */
    SUBGHZ_PROTO_NICE_FLO,    /* 12..24-bit, 2:1 */
    SUBGHZ_PROTO_HOLTEK,      /* 40-bit, header nibble 0x5 */
    SUBGHZ_PROTO_GATE_TX,     /* 24-bit, 2:1, start bit = te_long */
    SUBGHZ_PROTO_ANSONIC,     /* 12-bit, 2:1 inverted, FM */
    SUBGHZ_PROTO_DOITRAND,    /* 37-bit, 2:1, te 400/1100 */
    SUBGHZ_PROTO_PHOENIX_V2,  /* 52-bit, 2:1 inverted, te 427/853 */
    SUBGHZ_PROTO_CLEMSA,      /* 18-bit, 3:1-shape (HIGH-first), ratio 7:1 */
    SUBGHZ_PROTO_COUNT,
} subghz_proto_id_t;

typedef struct {
    subghz_proto_id_t proto;
    uint64_t key;       /* decoded data word */
    uint16_t bits;      /* number of data bits */
    uint32_t te;        /* base Te (us): per-capture for Princeton, fixed others */
} subghz_decoded_t;

const char       *subghz_protocol_name(subghz_proto_id_t id);
subghz_proto_id_t subghz_protocol_from_name(const char *name);

/* A protocol's native modulation: true = FM/2FSK (Ansonic), false = AM/OOK (all
 * others here). Used to reject a Key File whose stored preset modulation does
 * not match the protocol (a syntactically valid but internally inconsistent
 * file would otherwise transmit under the wrong modulation). */
bool subghz_protocol_is_fm(subghz_proto_id_t id);

/* Try the known protocols of the capture's modulation against one frame.
 * Isolated sub-50 us glitches are repaired in a private decoder buffer;
 * caller-owned durations remain unchanged. Returns true and fills *out on the
 * first match; otherwise sets out->proto = UNKNOWN and returns false.
 * durations: signed us, +high/-low,
 * starting on a mark. `fm` selects the protocol set by modulation (as Flipper
 * registers protocols per-modulation): false = AM/OOK (Princeton, Linear,
 * Holtek, Gate-TX, CAME, Nice FLO), true = FM/2FSK (Ansonic). This keeps the
 * timing-overlapping Nice FLO (AM) and Ansonic (FM) from ever being candidates
 * for the same capture. */
bool subghz_protocol_decode(const int32_t *durations, size_t count, bool fm,
                            subghz_decoded_t *out);

/* Encode one frame of `proto` with `bits` data bits of `key` into out[cap].
 * `te` is the base Te in us; 0 => the protocol's default. Returns the element
 * count written, or 0 on bad args / insufficient capacity. */
size_t subghz_protocol_encode(subghz_proto_id_t proto, uint64_t key,
                              uint16_t bits, uint32_t te,
                              int32_t *out, size_t cap);

/* Default/representative bit count for a protocol (used by Brute Force). */
uint16_t subghz_protocol_default_bits(subghz_proto_id_t id);
