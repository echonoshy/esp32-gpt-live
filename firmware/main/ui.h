#pragma once
#include <stdbool.h>
void ui_init(void);
void ui_set(const char *state, const char *detail);
void ui_battery(int percent, int volume);
void ui_wake(void);
void ui_tick(bool active);
void ui_show_setup(const char *ssid, bool website);
void ui_show_settings(const char *url);
void ui_show_guide(void);
