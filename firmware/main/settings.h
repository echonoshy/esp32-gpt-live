#pragma once
#include "esp_err.h"
typedef struct {
    int volume, brightness, screen_seconds, idle_seconds;
} settings_t;
void settings_init(void);
settings_t settings_get(void);
esp_err_t settings_save(settings_t value);
