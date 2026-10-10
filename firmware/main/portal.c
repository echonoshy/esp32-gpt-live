#include "portal.h"
#include "device.h"
#include "settings.h"
#include "wifi.h"
#include "wifi_profiles.h"
#include "ui.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static atomic_int armed_until;
static atomic_bool updating;
static char nonce[33];
extern const char page_start[] asm("_binary_settings_html_start");
extern const char page_end[] asm("_binary_settings_html_end");
bool portal_updating(void)
{
    return atomic_load(&updating);
}
void portal_arm(void)
{
    if (device_busy() || portal_updating()) {
        ui_set("SETTINGS UNAVAILABLE", "Reopen web setup");
        return;
    }
    atomic_store(&armed_until, esp_timer_get_time() / 1000000 + 300);
    char ip[20], detail[96];
    voice_wifi_ip(ip, sizeof(ip));
    snprintf(detail, sizeof(detail), "http://%s/", ip);
    ui_show_settings(detail);
}
static esp_err_t error(httpd_req_t *r, const char *status, const char *message)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, message);
}
static bool authorized(httpd_req_t *r)
{
    char supplied[40];
    return httpd_req_get_hdr_value_str(r, "X-Device-Nonce", supplied, sizeof(supplied)) == ESP_OK &&
           strcmp(supplied, nonce) == 0;
}
static esp_err_t root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "frame-ancestors 'none'");
    return httpd_resp_send(r, page_start, page_end - page_start - 1);
}
static esp_err_t info(httpd_req_t *r)
{
    settings_t p = settings_get();
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "nonce", nonce);
    cJSON_AddStringToObject(j, "state", device_state());
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(j, "battery", device_battery());
    cJSON_AddNumberToObject(j, "volume", p.volume);
    cJSON_AddNumberToObject(j, "brightness", p.brightness);
    cJSON_AddNumberToObject(j, "screen_seconds", p.screen_seconds);
    cJSON_AddNumberToObject(j, "idle_seconds", p.idle_seconds);
    cJSON_AddNumberToObject(j, "mode", p.mode);
    cJSON_AddNumberToObject(j, "voice", p.voice);
    cJSON_AddNumberToObject(j, "duplex", p.duplex);
    wifi_profiles_t profiles;
    if (wifi_profiles_load(&profiles) == ESP_OK) {
        cJSON *networks = cJSON_AddArrayToObject(j, "wifi_networks");
        for (unsigned i = 0; i < profiles.count; ++i)
            cJSON_AddItemToArray(networks, cJSON_CreateString(profiles.entries[i].ssid));
    }
    memset(&profiles, 0, sizeof(profiles));
    device_add_diagnostics(j);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s)
        return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, s);
    free(s);
    return e;
}
static cJSON *read_json(httpd_req_t *r)
{
    char content_type[40];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", content_type, sizeof(content_type)) !=
            ESP_OK ||
        strcmp(content_type, "application/json"))
        return NULL;
    if (r->content_len <= 0 || r->content_len > 512)
        return NULL;
    char body[513];
    int used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0)
            return NULL;
        used += n;
    }
    body[used] = 0;
    cJSON *j = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    return j;
}
static int integer(cJSON *j, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsNumber(v) && v->valuedouble == v->valueint ? v->valueint : -1;
}
static esp_err_t save(httpd_req_t *r)
{
    if (!authorized(r))
        return error(r, "403 Forbidden", "Refresh this page and retry.");
    if (portal_updating())
        return error(r, "409 Conflict", "Update in progress. Save settings after the update.");
    cJSON *j = read_json(r);
    settings_t p = {.volume = integer(j, "volume"),
                    .brightness = integer(j, "brightness"),
                    .screen_seconds = integer(j, "screen_seconds"),
                    .idle_seconds = integer(j, "idle_seconds"),
                    .mode = integer(j, "mode"),
                    .voice = integer(j, "voice"),
                    .duplex = 1};
    cJSON_Delete(j);
    settings_t previous = settings_get();
    esp_err_t e = settings_save(p);
    if (e != ESP_OK)
        return error(r, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error",
                     "Could not save. Check your settings and retry.");
    bool voice_changed = p.voice != previous.voice;
    device_apply_settings(voice_changed);
    return error(r, "200 OK", voice_changed ? "Voice saved. Clearing context and starting a new session."
                                            : "Settings saved. Idle timeout applies to the next session.");
}
static esp_err_t netcheck(httpd_req_t *r)
{
    if (!authorized(r))
        return error(r, "403 Forbidden", "Refresh settings first");
    if (device_busy() || portal_updating())
        return error(r, "409 Conflict", "Reopen Device > Web setup / update on the device.");
    device_netcheck();
    return error(r, "200 OK", "12 second silent network check queued; no microphone or GPT");
}
static void restart(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}
static esp_err_t wifi(httpd_req_t *r)
{
    if (!authorized(r))
        return error(r, "403 Forbidden", "Refresh this page and retry.");
    if (device_busy())
        return error(r, "409 Conflict", "Reopen Device > Web setup / update on the device.");
    cJSON *j = read_json(r), *ssid = cJSON_GetObjectItem(j, "ssid"),
          *password = cJSON_GetObjectItem(j, "password"),
          *forget = cJSON_GetObjectItem(j, "forget");
    bool removing = cJSON_IsString(forget);
    esp_err_t e = removing ? wifi_profiles_forget(forget->valuestring)
                          : cJSON_IsString(ssid) && cJSON_IsString(password)
                                ? voice_wifi_save(ssid->valuestring, password->valuestring)
                                : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (e != ESP_OK)
        return error(r, "400 Bad Request", e == ESP_ERR_NO_MEM
                     ? "8 networks saved. Forget one first."
                     : "Could not save Wi-Fi. Check your input.");
    atomic_store(&updating, true);
    ui_show_guide();
    ui_set("WI-FI SAVED", "Restarting device");
    error(r, "200 OK", removing ? "Network forgotten. Restarting device."
                               : "Wi-Fi saved; other networks kept. Restarting device.");
    xTaskCreate(restart, "restart", 2048, NULL, 4, NULL);
    return ESP_OK;
}
static esp_err_t update(httpd_req_t *r)
{
    if (!authorized(r))
        return error(r, "403 Forbidden", "Refresh this page and retry.");
    if (esp_timer_get_time() / 1000000 >= atomic_load(&armed_until))
        return error(r, "403 Forbidden", "Open Device > Web setup / update on the device.");
    atomic_store(&updating, true);
    if (device_busy()) {
        atomic_store(&updating, false);
        return error(r, "409 Conflict", "Reopen Device > Web setup / update on the device.");
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot || r->content_len < 1024 || r->content_len > slot->size) {
        atomic_store(&updating, false);
        return error(r, "400 Bad Request", "Invalid firmware size.");
    }
    char buffer[1024];
    const size_t prefix_size =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    int total = 0;
    while (total < prefix_size) {
        int n = httpd_req_recv(r, buffer + total, prefix_size - total);
        if (n <= 0) {
            atomic_store(&updating, false);
            return error(r, "400 Bad Request", "Upload interrupted. Current firmware is unchanged.");
        }
        total += n;
    }
    esp_app_desc_t description;
    memcpy(&description, buffer + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(description));
    if ((uint8_t)buffer[0] != ESP_IMAGE_HEADER_MAGIC ||
        description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(description.project_name, "passport-gpt-live", sizeof(description.project_name)) !=
            0) {
        atomic_store(&updating, false);
        return error(r, "400 Bad Request", "Select the passport-gpt-live.bin application image for this device.");
    }
    esp_ota_handle_t handle;
    esp_err_t e = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (e != ESP_OK) {
        atomic_store(&updating, false);
        return error(r, "500 Internal Server Error", "Could not prepare update. Restart the device and retry.");
    }
    ui_wake();
    ui_show_guide();
    ui_set("UPDATING", "Keep power connected");
    e = esp_ota_write(handle, buffer, total);
    int last_percent = -10;
    while (e == ESP_OK && total < r->content_len) {
        int remaining = r->content_len - total;
        int n = httpd_req_recv(r, buffer, remaining > sizeof(buffer) ? sizeof(buffer) : remaining);
        if (n <= 0) {
            e = ESP_FAIL;
            break;
        }
        e = esp_ota_write(handle, buffer, n);
        if (e != ESP_OK)
            break;
        total += n;
        int percent = total * 100 / r->content_len;
        if (percent / 10 != last_percent / 10) {
            char detail[64];
            snprintf(detail, sizeof(detail), "Uploaded %d%%\nKeep power connected", percent);
            ui_set("UPDATING", detail);
            last_percent = percent;
        }
    }
    if (e == ESP_OK)
        e = esp_ota_end(handle);
    else
        esp_ota_abort(handle);
    if (e == ESP_OK)
        e = esp_ota_set_boot_partition(slot);
    if (e != ESP_OK) {
        atomic_store(&updating, false);
        ui_set("UPDATE FAILED", "Firmware unchanged\nRetry the update");
        return error(r, "400 Bad Request", "Upload or validation failed. Current firmware is unchanged.");
    }
    atomic_store(&armed_until, 0);
    ui_set("UPDATE COMPLETE", "Restarting device");
    error(r, "200 OK", "Firmware verified and saved. Restarting. Refresh this page shortly.");
    xTaskCreate(restart, "restart", 2048, NULL, 4, NULL);
    return ESP_OK;
}
esp_err_t portal_start(void)
{
    snprintf(nonce, sizeof(nonce), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 6144;
    c.max_open_sockets = 2;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 10;
    httpd_handle_t server = NULL;
    esp_err_t e = httpd_start(&server, &c);
    if (e != ESP_OK)
        return e;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = save},
        {.uri = "/api/netcheck", .method = HTTP_POST, .handler = netcheck},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi},
        {.uri = "/api/update", .method = HTTP_POST, .handler = update}};
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) {
            httpd_stop(server);
            return e;
        }
    }
    return ESP_OK;
}
