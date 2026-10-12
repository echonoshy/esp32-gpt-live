#pragma once
#include <stdbool.h>
#include "esp_err.h"
bool firmware_update_active(void);
esp_err_t firmware_update_start(void);
