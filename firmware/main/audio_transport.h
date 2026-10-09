#pragma once
#include "adpcm.h"
#include <stdbool.h>
#include <string.h>

// Four independent 20 ms packets fit below the existing 1200-byte WS buffer.
#define UPLINK_BATCH_FRAMES 4
typedef struct {
    uint8_t packets[UPLINK_BATCH_FRAMES * ADPCM_BYTES];
    unsigned count;
} uplink_batch_t;
typedef bool (*uplink_send_fn)(const uint8_t *bytes, unsigned frames);

static inline bool uplink_flush(uplink_batch_t *batch, uplink_send_fn send)
{
    if (!batch->count)
        return true;
    bool ok = send(batch->packets, batch->count);
    // Failed/partially written WS frames must never be retried as a new frame.
    batch->count = 0;
    return ok;
}

static inline bool uplink_append(uplink_batch_t *batch, const uint8_t *packet, uplink_send_fn send)
{
    memcpy(batch->packets + batch->count * ADPCM_BYTES, packet, ADPCM_BYTES);
    return ++batch->count < UPLINK_BATCH_FRAMES || uplink_flush(batch, send);
}

#define PLAYBACK_FRAMES 128
#define PLAYBACK_LIVE_PREFILL 12
#define PLAYBACK_PTT_PREFILL 40
#define PLAYBACK_PTT_REFILL 60
typedef struct {
    uint32_t epoch, since;
    bool playing, waiting;
    unsigned target;
} playback_gate_t;

static inline void playback_gate_reset(playback_gate_t *g, uint32_t epoch, bool live)
{
    *g = (playback_gate_t){.epoch = epoch,
                           .target = live ? PLAYBACK_LIVE_PREFILL : PLAYBACK_PTT_PREFILL};
}

// Preserve ordering; short completed replies never wait for a full prefill.
static inline bool playback_gate_ready(playback_gate_t *g, unsigned queued, bool live, bool ended,
                                       uint32_t now)
{
    if (!queued) {
        if (g->playing && !ended && !live)
            g->target = PLAYBACK_PTT_REFILL;
        g->playing = g->waiting = false;
        return false;
    }
    if (g->playing)
        return true;
    if (!g->waiting) {
        g->waiting = true;
        g->since = now;
    }
    if (queued >= g->target || ended || now - g->since >= (live ? 350u : 1500u)) {
        g->playing = true;
        g->waiting = false;
    }
    return g->playing;
}
