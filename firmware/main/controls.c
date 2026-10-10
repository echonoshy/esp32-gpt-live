#include "controls.h"
#define BACK_HOLD 600u
#define REPEAT_START 500u
#define REPEAT_INTERVAL 150u

uint32_t controls_update(controls_t *s, uint8_t keys, uint32_t now, bool navigation)
{
    uint32_t out = 0;
    if (!s->active) {
        if (keys == KEY_NONE || keys == KEY_INVALID)
            return 0;
        *s = (controls_t){.active = true,
                          .navigation = navigation,
                          .origin = keys,
                          .started = now,
                          .repeat_at = now + REPEAT_START};
        // Chords stay inert until every key is released.
        if (keys != KEY_UP && keys != KEY_DOWN && keys != KEY_OK) {
            s->consumed = true;
            return 0;
        }
        out = CONTROL_WAKE;
        if (keys == KEY_UP || keys == KEY_DOWN) {
            s->adjusted = true;
            out |= keys == KEY_UP ? CONTROL_UP : CONTROL_DOWN;
        }
    }
    if (keys == KEY_NONE) {
        if (!s->consumed && s->origin == KEY_OK)
            out |= now - s->started >= BACK_HOLD ? CONTROL_BACK : CONTROL_SELECT;
        if (s->adjusted && !s->navigation)
            out |= CONTROL_SAVE_VOLUME;
        *s = (controls_t){0};
        return out;
    }
    if (s->consumed)
        return out;
    if (keys != s->origin) {
        s->consumed = true;
        return out;
    }
    if (s->origin == KEY_OK) {
        if (now - s->started >= BACK_HOLD) {
            out |= CONTROL_BACK;
            s->consumed = true; // Returning screens cannot turn this release into a tap.
        }
    } else if ((int32_t)(now - s->repeat_at) >= 0) {
        out |= s->origin == KEY_UP ? CONTROL_UP : CONTROL_DOWN;
        s->repeat_at = now + REPEAT_INTERVAL;
    }
    return out;
}
