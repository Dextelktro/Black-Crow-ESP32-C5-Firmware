#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================
 * Sub-GHz raw edge capture on the RMT RX peripheral.
 *
 * Captures the CC1101 GDO0 asynchronous-serial data stream (configured by the
 * OOK preset: IOCFG0=0x0D, PKTCTRL0=0x32) as a list of signed microsecond
 * durations, Flipper ".sub" RAW_Data convention: a POSITIVE value is a high
 * level (carrier/mark), a NEGATIVE value is a low level (space). The list
 * starts on a mark and ends on a mark (leading/trailing idle trimmed).
 *
 * Memory discipline: the RMT ping-pong symbol buffers live in internal RAM
 * (DMA/ISR reachable) and are allocated on start / freed on stop; the flattened
 * duration buffer lives in PSRAM (task-only). An RMT RX channel on GPIO24 is
 * allocated lazily on start and deleted on stop (IR and Sub-GHz are mutually
 * exclusive edge features, so the one spare RX channel is shared over time).
 * ============================================================ */

/* Flipper caps raw signals at 1024 timings; match that. */
#define SUBGHZ_RAW_MAX_TIMINGS 1024

/* Delivered from an internal task (NOT an ISR) once per captured FRAME.
 * `durations` is normalized: signs strictly alternate (consecutive same-level
 * RMT fragments are merged) and the list starts and ends on a mark.
 *
 * Framing is two-layered (see SUBGHZ_RX_MAX_NS vs SUBGHZ_FRAME_GAP_US in the
 * .c): the RMT hardware ends a reception when any level exceeds the max
 * recordable duration, and then the software splits that reception into frames
 * at any low whose length reaches the software frame-gap. So one hardware
 * reception can yield several callback invocations (one per complete frame) for
 * protocols whose inter-frame gap is shorter than the hardware limit.
 *
 * `truncated` is true only for the trailing frame of a reception that filled the
 * RMT user buffer (edges may have been dropped, so that frame is not known to be
 * complete); gap-closed frames are delivered with truncated = false. */
typedef void (*subghz_capture_cb_t)(const int32_t *durations, size_t count,
                                    bool truncated, void *ctx);

/* Start/stop continuous raw capture. The CC1101 must already be in async RX
 * (the caller strobes SRX after arming). Returns ESP_ERR_NOT_FOUND if no RMT
 * RX channel is free. */
esp_err_t subghz_capture_start(subghz_capture_cb_t cb, void *ctx);
esp_err_t subghz_capture_stop(void);
bool      subghz_capture_active(void);

/* Diagnostic counters — let data-loss / overload be measured instead of guessed
 * (no per-edge logging needed). Cleared on each subghz_capture_start(). */
typedef struct {
    uint32_t sessions;            /* capture sessions started            */
    uint32_t bursts;              /* frames delivered to the callback    */
    uint32_t received_symbols;    /* total RMT symbols received          */
    uint32_t buffer_overflow;     /* event-queue overflow occurrences    */
    uint32_t truncated;           /* receptions that filled the buffer   */
    uint32_t rmt_restarts;        /* rmt_receive() re-arms (ping-pong)   */
    uint32_t max_cb_latency_us;   /* worst callback processing time      */
    uint32_t frame_splits;        /* software frame-gap splits performed  */
} subghz_capture_stats_t;

void subghz_capture_get_stats(subghz_capture_stats_t *out);
