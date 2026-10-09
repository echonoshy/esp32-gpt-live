#pragma once
#include <stdbool.h>
#include "cJSON.h"
bool device_busy(void);
const char *device_state(void);
int device_battery(void);
void device_apply_settings(void);
void device_add_diagnostics(cJSON *root);
void device_netcheck(void);
