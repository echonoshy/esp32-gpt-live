#pragma once
#include <stdbool.h>
void ui_init(void);
void ui_set(const char *state, const char *detail);
void ui_level(int level);
void ui_caption(bool user, const char *text);
void ui_clear_captions(void);
void ui_battery(int percent, int volume);
void ui_wake(void);
void ui_tick(bool active);
