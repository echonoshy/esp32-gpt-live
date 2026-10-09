#pragma once
#include <stdbool.h>
typedef enum { CONTROL_UP, CONTROL_DOWN, CONTROL_OK } control_key_t;
typedef enum { CONTROL_PRESS, CONTROL_RELEASE, CONTROL_LONG } control_event_t;
typedef enum {
    CONTROL_NONE,
    CONTROL_WAKE,
    CONTROL_SETTINGS,
    CONTROL_LIVE,
    CONTROL_PREPARE,
    CONTROL_RELEASE_PTT,
    CONTROL_SELECT,
    CONTROL_BACK
} control_action_t;
typedef struct {
    bool pressed, navigation, held;
} control_press_t;
typedef struct {
    control_press_t keys[3];
} controls_t;
// Bind each press to its original screen, so exiting a menu cannot send speech on release.
control_action_t controls_event(controls_t *state, control_key_t key, control_event_t event,
                                bool navigation);
