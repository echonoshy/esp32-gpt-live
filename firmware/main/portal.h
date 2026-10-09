#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t portal_start(void);
void portal_arm(void);
bool portal_updating(void);
