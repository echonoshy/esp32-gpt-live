// cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main -Ifirmware/components/bsp/include
// scripts/test_controls.c firmware/main/controls.c -o /tmp/passport-controls
#include "controls.h"
#include "bsp_button_ladder.h"
#include <assert.h>
#include <stdio.h>
static controls_t s;
static uint32_t step(int keys, uint32_t ms, bool nav)
{
    return controls_update(&s, keys, ms, nav);
}
int main(void)
{
    assert(bsp_button_decode_mv(0) == KEY_UP);
    assert(bsp_button_decode_mv(212) == KEY_MENU);
    assert(bsp_button_decode_mv(300) == KEY_DOWN);
    assert(bsp_button_decode_mv(595) == KEY_OK);
    assert(bsp_button_decode_mv(2500) == KEY_NONE);
    assert(bsp_button_decode_mv(260) == KEY_INVALID);
    // No capture/action at key-down: a return gesture cannot start or submit speech.
    assert(step(KEY_OK, 0, false) == CONTROL_WAKE);
    assert(step(0, 100, false) == CONTROL_SELECT);
    assert(step(KEY_OK, 200, false) == CONTROL_WAKE);
    assert(step(KEY_OK, 799, false) == 0);
    assert(step(KEY_OK, 800, false) == CONTROL_BACK);
    assert(step(KEY_OK, 900, true) == 0);
    assert(step(0, 1000, true) == 0); // No home selection on release after returning.
    // Same hold-to-back everywhere, including settings and QR pages.
    step(KEY_OK, 1200, true);
    assert(step(KEY_OK, 1800, true) == CONTROL_BACK);
    assert(step(0, 1900, false) == 0);
    // Missed timer polls still classify a long release as back, never as a tap.
    step(KEY_OK, 2000, true);
    assert(step(0, 2600, true) == CONTROL_BACK);
    step(KEY_OK, 2700, true);
    assert(step(0, 3299, true) == CONTROL_SELECT);
    // Chords and partial releases stay inert; the next clean gesture works.
    assert(step(KEY_MENU, 4000, false) == 0);
    assert(step(KEY_MENU, 5000, false) == 0);
    assert(step(KEY_OK, 5100, false) == 0);
    assert(step(0, 5150, false) == 0);
    step(KEY_OK, 5200, false);
    assert(step(KEY_MENU, 5250, false) == 0);
    assert(step(KEY_OK, 5300, false) == 0);
    assert(step(0, 5400, false) == 0);
    step(KEY_INVALID, 5500, true);
    assert(step(KEY_OK, 5600, true) == CONTROL_WAKE); // An idle ADC error cannot swallow a press.
    assert(step(KEY_INVALID, 5650, true) == 0);
    assert(step(0, 5700, true) == 0); // Ambiguity within an active gesture still cancels it.
    step(KEY_OK, 5800, true);
    assert(step(0, 5900, true) == CONTROL_SELECT);
    // Navigation and volume respond immediately and repeat on hold.
    assert(step(KEY_DOWN, 6000, false) == (CONTROL_WAKE | CONTROL_DOWN));
    assert(step(KEY_DOWN, 6499, false) == 0);
    assert(step(KEY_DOWN, 6500, false) == CONTROL_DOWN);
    assert(step(KEY_DOWN, 6650, false) == CONTROL_DOWN);
    assert(step(0, 6700, false) == CONTROL_SAVE_VOLUME);
    assert(step(0, 6800, false) == 0);
    assert(step(KEY_UP, 7000, true) == (CONTROL_WAKE | CONTROL_UP));
    assert(step(KEY_UP, 7500, true) == CONTROL_UP);
    assert(step(0, 7600, false) == 0); // Bound to initial navigation context.
    step(KEY_DOWN, 8000, false);
    assert(step(KEY_MENU, 8100, false) == 0);
    assert(step(KEY_DOWN, 8700, false) == 0);
    assert(step(0, 8800, false) == CONTROL_SAVE_VOLUME);
    // Unsigned elapsed-time arithmetic also handles the millisecond clock wrapping.
    step(KEY_OK, 0xffffff00u, false);
    assert(step(KEY_OK, 0x158, false) == CONTROL_BACK);
    assert(step(0, 0x180, true) == 0);
    puts("PASS: single-key tap/back, hold release ownership, chord rejection, repeat/save, clock "
         "wrap");
}
