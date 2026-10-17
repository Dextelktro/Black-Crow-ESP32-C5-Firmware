#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================
 * IR protocol encode / decode.
 *
 * Works in terms of a microsecond timing list: alternating
 * mark/space durations, always starting with a mark (carrier ON).
 * This is the lingua franca between the RMT hardware layer
 * (ir_rmt) and the higher-level feature code (ir.c, ir_db.c).
 *
 * Carrier handling: each protocol carries its own modulation
 * frequency (NEC/Samsung = 38 kHz, Sony = 40 kHz, RC5/RC6 =
 * 36 kHz, RCA = 56 kHz). ir_encode() reports it so the caller
 * can configure the RMT carrier before transmitting.
 * ============================================================ */

typedef enum {
    IR_PROTO_UNKNOWN = 0,
    IR_PROTO_NEC,
    IR_PROTO_NECEXT,
    IR_PROTO_NEC42,
    IR_PROTO_SAMSUNG32,
    IR_PROTO_SONY,       /* SIRC — .bits selects 12 / 15 / 20 */
    IR_PROTO_RC5,
    IR_PROTO_RC6,
    IR_PROTO_RCA,
    IR_PROTO_PIONEER,
    IR_PROTO_KASEIKYO,
    IR_PROTO_RAW,
    IR_PROTO_COUNT,
} ir_proto_t;

typedef struct {
    ir_proto_t proto;
    uint32_t   address;
    uint32_t   command;
    uint8_t    bits;        /* protocol-specific bit count (Sony: 12/15/20) */
    uint32_t   carrier_hz;  /* filled by ir_encode(); 0 = default 38 kHz    */
    float      duty;        /* filled by ir_encode(); 0 = default 0.33      */
} ir_message_t;

/* Max timings a single parsed frame can produce (worst case: Manchester
 * RC6 ~ 2*21 half-bits + leader). Raw captures can be larger — see ir_rmt. */
#define IR_ENCODE_MAX_TIMINGS 320

const char *ir_proto_name(ir_proto_t p);
ir_proto_t  ir_proto_from_name(const char *s);

/* Encode a parsed message into a microsecond timing list.
 * Returns the number of timings written (0 on failure / unsupported).
 * On success *carrier_hz_out and *duty_out hold the protocol carrier. */
size_t ir_encode(const ir_message_t *msg, uint16_t *out, size_t out_cap,
                 uint32_t *carrier_hz_out, float *duty_out);

/* Best-effort decode of a raw timing list into a parsed message.
 * Returns true if a known protocol matched; otherwise *out is left
 * with proto = IR_PROTO_UNKNOWN and the caller should treat it as RAW. */
bool ir_decode(const uint16_t *timings, size_t count, ir_message_t *out);
