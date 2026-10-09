#pragma once
#include <stddef.h>
#include <stdint.h>

// 2x (+6 dB) speech gain, with one-frame lookahead and ~320 ms recovery.
// Peak limiting happens before scaling, so no PCM samples are hard-clipped.
// Keep state on the speaker task; codec volume still controls final loudness.
typedef struct {
    int32_t gain_q15;
} playback_gain_t;

static inline void playback_gain_apply(playback_gain_t *state, int16_t *pcm, size_t count)
{
    if (!count)
        return;
    int32_t peak = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t magnitude = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (magnitude > peak)
            peak = magnitude;
    }
    const int32_t desired = 65536; // +6.02 dB
    int32_t allowed = peak > 15000 ? (30000 * 32768) / peak : desired;
    if (!state->gain_q15)
        state->gain_q15 = desired;
    if (allowed < state->gain_q15)
        state->gain_q15 = allowed;
    else
        state->gain_q15 += (allowed - state->gain_q15 + 15) / 16;
    for (size_t i = 0; i < count; ++i)
        pcm[i] = (int16_t)(((int32_t)pcm[i] * state->gain_q15) / 32768);
}
