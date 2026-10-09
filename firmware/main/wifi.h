#pragma once
#include "esp_err.h"
#include <stddef.h>
esp_err_t voice_wifi_start(void);
esp_err_t voice_wifi_save(const char *ssid, const char *password);
void voice_wifi_ip(char *output, size_t size);
