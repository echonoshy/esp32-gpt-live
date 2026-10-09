#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { KEY_NONE = 0, KEY_UP = 1, KEY_DOWN = 2, KEY_OK = 4, KEY_MENU = 6, KEY_INVALID = 8 };
enum {
    CONTROL_WAKE = 1u << 0,
    CONTROL_PREPARE = 1u << 1,
    CONTROL_RELEASE_PTT = 1u << 2,
    CONTROL_CANCEL_CAPTURE = 1u << 3,
    CONTROL_STOP = 1u << 4,
    CONTROL_SETTINGS = 1u << 5,
    CONTROL_UP = 1u << 6,
    CONTROL_DOWN = 1u << 7,
    CONTROL_SAVE_VOLUME = 1u << 8,
    CONTROL_SELECT = 1u << 9,
    CONTROL_BACK = 1u << 10
};
typedef struct {
    uint8_t origin;
    bool active, navigation, chord, consumed, held, adjusted;
    uint32_t started, chord_started, repeat_at;
} controls_t;
// Called with a debounced full key mask; one gesture remains bound to its initial screen.
uint32_t controls_update(controls_t *s, uint8_t keys, uint32_t now, bool navigation);
