#include "wifi_setup.h"
#include "wifi.h"
#include "wifi_profiles.h"
#include "ui.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/lock.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SCAN_MAX 24
static _lock_t state_lock;
static char nonce[33];
static const char *phase = "idle", *failure = "";
static bool scanning;
static const char *scan_error = "";
static char connected_ssid[33], connected_ip[20];
static struct { char ssid[33]; int rssi; bool secured; bool supported; } networks[SCAN_MAX];
static unsigned network_count;

static esp_err_t json_reply(httpd_req_t *r, cJSON *j)
{
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, body);
    free(body);
    return e;
}
bool wifi_setup_authorized(httpd_req_t *r)
{
    char supplied[40];
    return httpd_req_get_hdr_value_str(r, "X-Setup-Nonce", supplied, sizeof(supplied)) == ESP_OK &&
           !strcmp(supplied, nonce);
}
static esp_err_t state(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    _lock_acquire(&state_lock);
    cJSON_AddStringToObject(j, "nonce", nonce);
    cJSON_AddStringToObject(j, "state", phase);
    cJSON_AddStringToObject(j, "error", failure);
    cJSON_AddStringToObject(j, "ssid", connected_ssid);
    cJSON_AddStringToObject(j, "ip", connected_ip);
    cJSON_AddBoolToObject(j, "connected", voice_wifi_connected());
    _lock_release(&state_lock);
    return json_reply(r, j);
}
static esp_err_t scan_results(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    _lock_acquire(&state_lock);
    cJSON_AddBoolToObject(j, "scanning", scanning);
    cJSON_AddStringToObject(j, "error", scan_error);
    cJSON *list = cJSON_AddArrayToObject(j, "networks");
    for (unsigned i = 0; i < network_count; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(n, "secured", networks[i].secured);
        cJSON_AddBoolToObject(n, "supported", networks[i].supported);
        cJSON_AddItemToArray(list, n);
    }
    _lock_release(&state_lock);
    return json_reply(r, j);
}
static void scan_task(void *unused)
{
    wifi_ap_record_t *records = calloc(SCAN_MAX, sizeof(*records));
    uint16_t count = SCAN_MAX;
    wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t e = records ? esp_wifi_scan_start(&config, true) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = esp_wifi_scan_get_ap_records(&count, records);
    // Always release driver-owned scan storage, including failures.
    esp_wifi_clear_ap_list();
    _lock_acquire(&state_lock);
    network_count = 0;
    if (e == ESP_OK) {
        for (unsigned i = 0; i < count; i++) {
            if (!records[i].ssid[0]) continue;
            bool secured = records[i].authmode != WIFI_AUTH_OPEN;
            bool duplicate = false;
            for (unsigned k = 0; k < network_count; k++)
                if (!strncmp(networks[k].ssid, (char *)records[i].ssid, 32) &&
                    networks[k].secured == secured) duplicate = true;
            if (duplicate) continue;
            unsigned k = network_count++;
            memcpy(networks[k].ssid, records[i].ssid, 32);
            networks[k].ssid[32] = 0;
            networks[k].rssi = records[i].rssi;
            networks[k].secured = secured;
            networks[k].supported = records[i].authmode == WIFI_AUTH_OPEN ||
                records[i].authmode == WIFI_AUTH_WPA2_PSK ||
                records[i].authmode == WIFI_AUTH_WPA_WPA2_PSK ||
                records[i].authmode == WIFI_AUTH_WPA3_PSK ||
                records[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK;
        }
    }
    scan_error = e == ESP_OK ? "" : "Scan failed. Tap Refresh to retry.";
    scanning = false;
    _lock_release(&state_lock);
    free(records);
    voice_wifi_radio_give();
    vTaskDelete(NULL);
}
static esp_err_t scan_start(httpd_req_t *r)
{
    if (!wifi_setup_authorized(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Reload this page.");
    voice_wifi_setup_claim();
    if (!voice_wifi_radio_take()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Wi-Fi is busy. Please retry shortly.");
    }
    _lock_acquire(&state_lock);
    scanning = true; scan_error = "";
    _lock_release(&state_lock);
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        _lock_acquire(&state_lock); scanning = false; _lock_release(&state_lock);
        voice_wifi_radio_give();
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory to scan.");
    }
    return scan_results(r);
}
static void connect_task(void *arg)
{
    wifi_profile_t *p = arg;
    unsigned reason = 0;
    esp_err_t e = voice_wifi_test(p->ssid, p->password, &reason);
    const char *error = "";
    if (e == ESP_OK) {
        e = wifi_profiles_save(p->ssid, p->password);
        if (e != ESP_OK) error = e == ESP_ERR_NO_MEM ? "Eight networks saved. Forget one and retry." : "Connected, but saving failed. Retry.";
    } else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
               reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
        error = "Authentication failed. Check the password and router settings.";
    } else if (reason == WIFI_REASON_NO_AP_FOUND) {
        error = "Network not found. Check that the 2.4 GHz network is available.";
    } else error = "Could not obtain a connection and IP address. Check the network and retry.";
    if (e != ESP_OK) esp_wifi_disconnect();
    _lock_acquire(&state_lock);
    phase = e == ESP_OK ? "connected" : "failed";
    failure = error;
    if (e == ESP_OK) {
        strcpy(connected_ssid, p->ssid);
        voice_wifi_ip(connected_ip, sizeof(connected_ip));
    }
    _lock_release(&state_lock);
    memset(p, 0, sizeof(*p)); free(p);
    ui_show_guide();
    ui_set(e == ESP_OK ? "WI-FI CONNECTED" : "WI-FI NOT CONNECTED",
           e == ESP_OK ? "Finish on your phone" : "Check phone and retry");
    voice_wifi_radio_give();
    vTaskDelete(NULL);
}
esp_err_t wifi_setup_connect(httpd_req_t *r, const char *ssid, const char *password)
{
    size_t n = strlen(password);
    if (!ssid[0] || strlen(ssid) > 32 || (n && n < 8) || n > 64 ||
        (n == 64 && strspn(password, "0123456789abcdefABCDEF") != 64))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Check network name and password length.");
    wifi_profile_t *p = calloc(1, sizeof(*p));
    if (!p) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory.");
    strcpy(p->ssid, ssid); strcpy(p->password, password);
    voice_wifi_setup_claim();
    if (!voice_wifi_radio_take()) {
        memset(p, 0, sizeof(*p)); free(p);
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Wi-Fi is busy. Wait for scanning to finish and retry.");
    }
    _lock_acquire(&state_lock);
    phase = "connecting"; failure = ""; connected_ssid[0] = connected_ip[0] = 0;
    _lock_release(&state_lock);
    if (xTaskCreate(connect_task, "wifi_test", 4096, p, 3, NULL) != pdPASS) {
        memset(p, 0, sizeof(*p)); free(p);
        _lock_acquire(&state_lock); phase = "failed"; failure = "Not enough memory. Retry."; _lock_release(&state_lock);
        voice_wifi_radio_give();
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start connection.");
    }
    httpd_resp_set_status(r, "202 Accepted");
    return state(r);
}
static void restart_task(void *unused)
{
    vTaskDelay(pdMS_TO_TICKS(1800)); esp_restart();
}
static esp_err_t finish(httpd_req_t *r)
{
    if (!wifi_setup_authorized(r)) return httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "Reload this page.");
    _lock_acquire(&state_lock);
    bool ok = !strcmp(phase, "connected") && voice_wifi_connected();
    _lock_release(&state_lock);
    if (!ok) { httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "Connect successfully before finishing."); }
    if (!voice_wifi_radio_take()) { httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "Wait for Wi-Fi work to finish."); }
    if (xTaskCreate(restart_task, "wifi_finish", 2048, NULL, 3, NULL) != pdPASS) {
        voice_wifi_radio_give();
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not restart. Retry.");
    }
    return httpd_resp_sendstr(r, "Connected and saved. Rejoin your usual Wi-Fi while the device restarts.");
}
void wifi_setup_register(httpd_handle_t server)
{
    snprintf(nonce, sizeof(nonce), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    const httpd_uri_t routes[] = {
        {.uri="/api/setup", .method=HTTP_GET, .handler=state},
        {.uri="/api/scan", .method=HTTP_GET, .handler=scan_results},
        {.uri="/api/scan", .method=HTTP_POST, .handler=scan_start},
        {.uri="/finish", .method=HTTP_POST, .handler=finish},
    };
    for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
}
