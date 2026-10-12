#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t portal_start(void);
bool portal_updating(void);

// Shared ownership serializes network maintenance and on-device firmware updates.
bool portal_update_lock(void);
void portal_update_unlock(void);
