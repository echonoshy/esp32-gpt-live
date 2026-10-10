// Host regression for packet batching and 500 ms playback admission in both modes.
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

    for (unsigned mode = 0; mode < 2; ++mode) {
        bool live = mode != 0;
        playback_gate_t gate;
        playback_gate_reset(&gate, 1, live);
        assert(!playback_gate_ready(&gate, 1, live, false, 100));
        // 24 frames are only 480 ms: both modes must wait for frame 25.
        assert(!playback_gate_ready(&gate, 24, live, false, 560));
        assert(playback_gate_ready(&gate, 25, live, false, 580));
        assert(playback_gate_ready(&gate, 1, live, false, 600));
        assert(!playback_gate_ready(&gate, 0, live, false, 620));
        assert(gate.target == 25);
        assert(!playback_gate_ready(&gate, 1, live, false, 640));
        assert(!playback_gate_ready(&gate, 24, live, false, 1100));
        assert(playback_gate_ready(&gate, 25, live, false, 1120));
        // A recovered 1.5 second burst drains in order without local catch-up loss.
        for (unsigned queued = 75; queued; queued--)
            assert(playback_gate_ready(&gate, queued, live, false, 1140 + (75 - queued) * 20));
        playback_gate_reset(&gate, 2, live);
        assert(gate.target == 25 && !gate.playing);
        assert(playback_gate_ready(&gate, 2, live, true, 2700));
        // Starved unfinished replies retain their bounded wait, including clock wrap.
        uint32_t wait = live ? 650 : 1500;
        playback_gate_reset(&gate, 3, live);
        assert(!playback_gate_ready(&gate, 1, live, false, UINT32_MAX - 100));
        assert(!playback_gate_ready(&gate, 2, live, false, wait - 102));
        assert(playback_gate_ready(&gate, 2, live, false, wait - 101));
    }
    puts("PASS: ordered batches, partial release, cancellation, failed-send non-replay; both modes 500 ms startup/refill, short replies, epoch reset, clock wrap, ordered recovery");
}
