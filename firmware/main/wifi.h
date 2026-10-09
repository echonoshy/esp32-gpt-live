#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
esp_err_t voice_wifi_start(void);
esp_err_t voice_wifi_save(const char *ssid, const char *password);
void voice_wifi_ip(char *output, size_t size);
bool voice_wifi_provisioning(void);
void voice_wifi_setup_next(void);
esp_err_t voice_wifi_request_setup(void);
bool voice_wifi_connected(void);
