// cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main -Ifirmware/components/bsp/include
// scripts/test_controls.c firmware/main/controls.c -o /tmp/passport-controls
#include "controls.h"
#include "bsp_button_ladder.h"
#include <assert.h>
#include <stdio.h>
static controls_t s;
static uint32_t step(int keys, int ms, bool nav)
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
    assert(step(KEY_OK, 0, false) & CONTROL_PREPARE);
    assert(step(0, 100, false) == (CONTROL_RELEASE_PTT | CONTROL_STOP));
    assert(step(KEY_OK, 200, false) & CONTROL_PREPARE);
    step(KEY_OK, 400, false);
    assert(step(0, 500, false) == CONTROL_RELEASE_PTT);
    // Early chord consumes pre-roll and suppresses both single-key actions.
    step(KEY_OK, 1000, false);
    assert(step(KEY_MENU, 1040, false) == CONTROL_CANCEL_CAPTURE);
    assert(step(KEY_MENU, 1800, false) == 0);
    assert(step(KEY_MENU, 1840, false) == CONTROL_SETTINGS);
    assert(step(KEY_DOWN, 1900, true) == 0);
    assert(step(0, 1950, true) == 0);
    // Short/aborted chords do nothing and cannot select after opening the menu.
    step(KEY_MENU, 2000, false);
    assert(step(KEY_OK, 2100, false) == 0);
    assert(step(0, 2150, false) == 0);
    // Late added keys never become a chord, even after a long recording.
    step(KEY_OK, 3000, false);
    step(KEY_OK, 3250, false);
    assert(!(step(KEY_MENU, 3300, false) & CONTROL_SETTINGS));
    assert(!(step(KEY_MENU, 4200, false) & CONTROL_SETTINGS));
    assert(step(KEY_DOWN, 4250, false) == CONTROL_RELEASE_PTT);
    assert(step(0, 4300, false) == 0);
    // Volume first adjustment after 80 ms, held repeat, exactly one save on release.
    step(KEY_DOWN, 5000, false);
    assert(step(KEY_DOWN, 5080, false) == CONTROL_DOWN);
    assert(step(KEY_MENU, 5100, false) == 0);
    assert(step(KEY_MENU, 5500, false) == CONTROL_DOWN);
    assert(step(KEY_MENU, 5650, false) == CONTROL_DOWN);
    assert(step(0, 5700, false) == CONTROL_SAVE_VOLUME);
    step(KEY_UP, 6000, false);
    assert(step(0, 6050, false) == (CONTROL_UP | CONTROL_SAVE_VOLUME));
    // A chord starting with DOWN must not decrement volume.
    step(KEY_DOWN, 7000, false);
    assert(step(KEY_MENU, 7040, false) == CONTROL_CANCEL_CAPTURE);
    assert(step(KEY_MENU, 7840, false) == CONTROL_SETTINGS);
    step(0, 7900, true);
    // Menu press belongs to its original screen; long OK back does not record/select.
    step(KEY_OK, 8000, true);
    assert(step(KEY_OK, 8500, true) == CONTROL_BACK);
    assert(step(KEY_OK, 8600, false) == 0);
    assert(step(0, 8700, false) == 0);
    step(KEY_OK, 9000, true);
    assert(step(0, 9050, true) == CONTROL_SELECT);
    // Clock wrap remains a valid long chord.
    s = (controls_t){0};
    step(KEY_MENU, 0xffffff00u, false);
    assert(step(KEY_MENU, 0x220, false) == CONTROL_SETTINGS);
    puts("PASS: ADC ladder, PTT/tap, 800ms chord, late chord rejection, repeats, menu ownership, "
         "clock wrap");
}
