#include "controls.h"
#define CHORD_WINDOW 80u
#define MENU_HOLD 800u
#define PTT_HOLD 200u
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
                          .repeat_at = now + REPEAT_START,
                          .chord = keys == KEY_MENU,
                          .chord_started = now};
        out = CONTROL_WAKE;
        if (keys == KEY_OK && !navigation)
            out |= CONTROL_PREPARE;
        if (navigation && (keys == KEY_UP || keys == KEY_DOWN)) {
            s->adjusted = true;
            out |= keys == KEY_UP ? CONTROL_UP : CONTROL_DOWN;
        }
    }
    if (keys == KEY_NONE) {
        if (!s->consumed && !s->chord) {
            if (s->origin == KEY_OK) {
                if (s->navigation) {
                    if (!s->held)
                        out |= CONTROL_SELECT;
                } else
                    out |= CONTROL_RELEASE_PTT | (now - s->started < PTT_HOLD ? CONTROL_STOP : 0);
            } else if (!s->navigation && (s->origin == KEY_UP || s->origin == KEY_DOWN)) {
                if (!s->adjusted)
                    out |= s->origin == KEY_UP ? CONTROL_UP : CONTROL_DOWN;
                out |= CONTROL_SAVE_VOLUME;
            }
        }
        *s = (controls_t){0};
        return out;
    }
    if (s->consumed)
        return out;
    // A later second key can never transform recording/volume into a menu gesture.
    if (!s->navigation && !s->chord && keys == KEY_MENU &&
        (s->origin == KEY_OK || s->origin == KEY_DOWN) && now - s->started <= CHORD_WINDOW) {
        s->chord = true;
        s->chord_started = now;
        out |= CONTROL_CANCEL_CAPTURE;
    }
    if (s->chord) {
        if (s->navigation || keys != KEY_MENU)
            s->consumed = true;
        else if (now - s->chord_started >= MENU_HOLD) {
            out |= CONTROL_SETTINGS;
            s->consumed = true;
        }
        return out;
    }
    if (keys == KEY_INVALID || !(keys & s->origin)) {
        if (s->origin == KEY_OK && !s->navigation)
            out |= CONTROL_RELEASE_PTT;
        if (s->adjusted && !s->navigation)
            out |= CONTROL_SAVE_VOLUME;
        s->consumed = true;
        return out;
    }
    if (s->origin == KEY_OK) {
        if (now - s->started >= (s->navigation ? 500u : PTT_HOLD) && !s->held) {
            s->held = true;
            if (s->navigation)
                out |= CONTROL_BACK;
        }
    } else if (s->origin == KEY_UP || s->origin == KEY_DOWN) {
        bool first = !s->adjusted && now - s->started >= CHORD_WINDOW;
        if (first || (int32_t)(now - s->repeat_at) >= 0) {
            out |= s->origin == KEY_UP ? CONTROL_UP : CONTROL_DOWN;
            s->adjusted = true;
            if (!first)
                s->repeat_at = now + REPEAT_INTERVAL;
        }
    }
    return out;
}
