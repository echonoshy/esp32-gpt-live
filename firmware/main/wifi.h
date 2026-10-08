#pragma once
#include "esp_err.h"
esp_err_t voice_wifi_start(void);
esp_err_t voice_wifi_save(const char *ssid, const char *password);
