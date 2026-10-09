// Host regression for packet batching and mode-specific playback admission.
#include "audio_transport.h"
#include <assert.h>
#include <stdio.h>
static unsigned calls, received, sizes[8];
static bool fail;
static bool send_frames(const uint8_t *bytes, unsigned frames)
{
    assert(frames > 0 && frames <= 4);
    sizes[calls++] = frames;
    for (unsigned i = 0; i < frames; i++)
        assert(bytes[i * ADPCM_BYTES] == received++);
    return !fail;
}
int main(void)
{
    uplink_batch_t batch = {0};
    for (unsigned i = 0; i < 11; i++) {
        uint8_t packet[ADPCM_BYTES] = {i};
        assert(uplink_append(&batch, packet, send_frames));
    }
    assert(calls == 2 && received == 8 && batch.count == 3);
    // Releasing OK flushes the last partial batch exactly once, before ptt_end.
    assert(uplink_flush(&batch, send_frames));
    assert(calls == 3 && received == 11 && sizes[2] == 3);
    assert(uplink_flush(&batch, send_frames) && calls == 3);
    uint8_t packet[ADPCM_BYTES] = {11};
    assert(uplink_append(&batch, packet, send_frames));
    fail = true;
    assert(!uplink_flush(&batch, send_frames) && batch.count == 0);
    assert(uplink_flush(&batch, send_frames) && calls == 4);
    // A cancelled partial batch cannot leak into the next utterance.
    assert(uplink_append(&batch, packet, send_frames));
    batch.count = 0;
    assert(uplink_flush(&batch, send_frames) && calls == 4);

    playback_gate_t gate;
    playback_gate_reset(&gate, 1, false);
    assert(!playback_gate_ready(&gate, 1, false, false, 100));
    assert(!playback_gate_ready(&gate, 39, false, false, 860));
    assert(playback_gate_ready(&gate, 40, false, false, 880));
    assert(playback_gate_ready(&gate, 1, false, false, 900));
    assert(!playback_gate_ready(&gate, 0, false, false, 920));
    assert(gate.target == 60);
    assert(!playback_gate_ready(&gate, 40, false, false, 1000));
    assert(playback_gate_ready(&gate, 60, false, false, 1400));
    playback_gate_reset(&gate, 2, false);
    assert(gate.target == 40 && !gate.playing);
    assert(playback_gate_ready(&gate, 2, false, true, 1420));
    playback_gate_reset(&gate, 3, false);
    assert(!playback_gate_ready(&gate, 1, false, false, UINT32_MAX - 100));
    assert(!playback_gate_ready(&gate, 2, false, false, 1398));
    assert(playback_gate_ready(&gate, 2, false, false, 1399));
    playback_gate_reset(&gate, 4, true);
    assert(gate.target == 12);
    assert(!playback_gate_ready(&gate, 3, true, false, 100));
    assert(playback_gate_ready(&gate, 12, true, false, 200));
    assert(!playback_gate_ready(&gate, 0, true, false, 300));
    assert(gate.target == 12);
    puts("PASS: ordered batches, partial release, cancellation, failed-send non-replay; PTT prefill/refill, short replies, epoch reset, clock wrap, live latency");
}
