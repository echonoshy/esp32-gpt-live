#pragma once
#include <stdbool.h>
bool device_busy(void);
const char *device_state(void);
int device_battery(void);
void device_apply_settings(void);
