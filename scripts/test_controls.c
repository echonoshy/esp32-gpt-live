// Host regression: cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main
// scripts/test_controls.c firmware/main/controls.c -o /tmp/passport-controls &&
// /tmp/passport-controls
#include "controls.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    controls_t state = {0};
    assert(controls_event(&state, CONTROL_UP, CONTROL_PRESS, false) == CONTROL_WAKE);
    assert(controls_event(&state, CONTROL_UP, CONTROL_LONG, false) == CONTROL_NONE);
    assert(controls_event(&state, CONTROL_UP, CONTROL_RELEASE, false) == CONTROL_SETTINGS);
    assert(controls_event(&state, CONTROL_DOWN, CONTROL_PRESS, false) == CONTROL_WAKE);
    assert(controls_event(&state, CONTROL_DOWN, CONTROL_RELEASE, false) == CONTROL_LIVE);
    // A short chat press only starts/cancels local pre-roll; it never opens a menu.
    assert(controls_event(&state, CONTROL_OK, CONTROL_PRESS, false) == CONTROL_PREPARE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_RELEASE, false) == CONTROL_RELEASE_PTT);
    assert(controls_event(&state, CONTROL_OK, CONTROL_RELEASE, false) == CONTROL_NONE);
    // Press stays bound to its original context, including after returning to chat.
    assert(controls_event(&state, CONTROL_OK, CONTROL_PRESS, true) == CONTROL_WAKE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_LONG, true) == CONTROL_BACK);
    assert(controls_event(&state, CONTROL_OK, CONTROL_LONG, false) == CONTROL_NONE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_RELEASE, false) == CONTROL_NONE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_PRESS, true) == CONTROL_WAKE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_RELEASE, true) == CONTROL_SELECT);
    assert(controls_event(&state, CONTROL_OK, CONTROL_PRESS, false) == CONTROL_PREPARE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_LONG, false) == CONTROL_NONE);
    assert(controls_event(&state, CONTROL_OK, CONTROL_RELEASE, true) == CONTROL_RELEASE_PTT);
    puts("PASS: key positions, short OK, single long-back, context-bound release");
}
