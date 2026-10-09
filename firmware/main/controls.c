#include "controls.h"
control_action_t controls_event(controls_t *state, control_key_t key, control_event_t event,
                                bool navigation)
{
    control_press_t *p = &state->keys[key];
    if (event == CONTROL_PRESS) {
        *p = (control_press_t){.pressed = true, .navigation = navigation};
        return key == CONTROL_OK && !navigation ? CONTROL_PREPARE : CONTROL_WAKE;
    }
    if (!p->pressed)
        return CONTROL_NONE;
    if (event == CONTROL_LONG) {
        if (key == CONTROL_OK && p->navigation && !p->held) {
            p->held = true;
            return CONTROL_BACK;
        }
        return CONTROL_NONE;
    }
    p->pressed = false;
    if (key == CONTROL_OK)
        return p->navigation ? (p->held ? CONTROL_NONE : CONTROL_SELECT) : CONTROL_RELEASE_PTT;
    return key == CONTROL_UP ? CONTROL_SETTINGS : CONTROL_LIVE;
}
