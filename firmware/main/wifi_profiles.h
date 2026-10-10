#pragma once

#include "esp_err.h"
#include <stdint.h>

#define WIFI_PROFILE_MAX 8
#define WIFI_CONFIG_NAMESPACE "gpt_live"

typedef struct {
    char ssid[33];
    char password[65];
} wifi_profile_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    wifi_profile_t entries[WIFI_PROFILE_MAX];
} wifi_profiles_t;

esp_err_t wifi_profiles_load(wifi_profiles_t *profiles);
esp_err_t wifi_profiles_save(const char *ssid, const char *password);
esp_err_t wifi_profiles_forget(const char *ssid);
