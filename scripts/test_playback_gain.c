#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "playback_gain.h"

int main(void)
{
    playback_gain_t state = {0};
    int16_t quiet[] = {0, 1000, -1000, 10000, -10000};
    playback_gain_apply(&state, quiet, 5);
    assert(quiet[0] == 0 && quiet[1] == 2000 && quiet[2] == -2000);
    assert(quiet[3] == 20000 && quiet[4] == -20000);
    int16_t loud[] = {INT16_MIN, INT16_MAX, 16000, -16000};
    playback_gain_apply(&state, loud, 4);
    assert(loud[0] >= -30000 && loud[1] <= 30000);
    assert(loud[2] > 0 && loud[3] == -loud[2]);
    int32_t limited = state.gain_q15;
    int16_t silence[480] = {0};
    playback_gain_apply(&state, silence, 480);
    assert(state.gain_q15 > limited && state.gain_q15 < 65536);
    for (int i = 0; i < 300; ++i)
        playback_gain_apply(&state, silence, 480);
    assert(state.gain_q15 == 65536);
    for (int i = 0; i < 480; ++i)
        assert(silence[i] == 0);
    // Exhaust the signed PCM range: no overflow, polarity reversal or clipping.
    for (int v = INT16_MIN; v <= INT16_MAX; ++v) {
        state.gain_q15 = 0;
        int16_t pcm = v;
        playback_gain_apply(&state, &pcm, 1);
        assert(pcm >= -30000 && pcm <= 30000);
        assert(v == 0 ? pcm == 0 : (v < 0 ? pcm < 0 : pcm > 0));
    }
    puts("playback gain tests passed");
}
