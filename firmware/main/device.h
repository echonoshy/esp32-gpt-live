#pragma once
#include <stdbool.h>
#include "cJSON.h"
bool device_busy(void);
const char *device_state(void);
int device_battery(void);
// A saved voice change also opens a fresh conversation in the current input mode.
void device_apply_settings(bool voice_changed);
void device_add_diagnostics(cJSON *root);
void device_netcheck(void);
