#pragma once
#include "esp_http_server.h"
#include <stdbool.h>
void wifi_setup_register(httpd_handle_t server);
bool wifi_setup_authorized(httpd_req_t *r);
esp_err_t wifi_setup_connect(httpd_req_t *r, const char *ssid, const char *password);
/* Shared radio ownership with saved-network retries. */
bool voice_wifi_radio_take(void);
void voice_wifi_radio_give(void);
void voice_wifi_setup_claim(void);
esp_err_t voice_wifi_test(const char *ssid, const char *password, unsigned *reason);
