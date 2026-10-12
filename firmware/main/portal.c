#include "portal.h"
#include "firmware_update.h"
#include "device.h"
#include "settings.h"
#include "wifi.h"
#include "wifi_profiles.h"
#include "ui.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static atomic_bool updating;
static char nonce[33];
extern const char page_start[] asm("_binary_settings_html_start");
extern const char page_end[] asm("_binary_settings_html_end");
bool portal_updating(void)
{
    return atomic_load(&updating) || firmware_update_active();
}
bool portal_update_lock(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&updating, &expected, true);
}
void portal_update_unlock(void) { atomic_store(&updating, false); }
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
                       "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "frame-ancestors 'none'");
    return httpd_resp_send(r, page_start, page_end - page_start - 1);
}
static void image_id(const esp_app_desc_t *description, char output[65])
{
    for (unsigned i = 0; i < 32; i++) snprintf(output + i * 2, 3, "%02x", description->app_elf_sha256[i]);
}
static esp_err_t info(httpd_req_t *r)
{
    settings_t p = settings_get();
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "nonce", nonce);
    cJSON_AddStringToObject(j, "boot_id", nonce);
    uint8_t mac[6]; char device_id[18];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(j, "device_id", device_id);
    char identity[65]; image_id(esp_app_get_description(), identity);
    cJSON_AddStringToObject(j, "image_id", identity);
    esp_ota_img_states_t ota_state;
    esp_err_t ota_result = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state);
    cJSON_AddStringToObject(j, "ota_state", ota_result != ESP_OK ? "unknown" :
        ota_state == ESP_OTA_IMG_VALID ? "valid" :
        ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify" : "unconfirmed");
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
        return error(r, "409 Conflict", "End the conversation and wait for any update to finish.");
    device_netcheck();
    return error(r, "200 OK", "12 second silent network check queued; no microphone or GPT");
}
static void restart(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}
static void setup_restart(void *unused)
{
    vTaskDelay(pdMS_TO_TICKS(1800));
    if (voice_wifi_request_setup() != ESP_OK) {
        atomic_store(&updating, false);
        ui_set("SETUP FAILED", "Use Network > Connect");
    }
    vTaskDelete(NULL);
}
static esp_err_t wifi_setup(httpd_req_t *r)
{
    if (!authorized(r)) return error(r, "403 Forbidden", "Refresh this page first.");
    if (device_busy() || portal_updating())
        return error(r, "409 Conflict", "Use Network > Connect Wi-Fi on the device.");
    if (!portal_update_lock())
        return error(r, "409 Conflict", "An update or restart is already in progress.");
    if (xTaskCreate(setup_restart, "wifi_setup", 3072, NULL, 3, NULL) != pdPASS) {
        atomic_store(&updating, false);
        return error(r, "500 Internal Server Error", "Could not open setup. Retry.");
    }
    return error(r, "200 OK", "Device restarting into setup. Scan the JOIN HOTSPOT code on its screen, join mic-on, then select your Wi-Fi on the setup page.");
}
static esp_err_t wifi(httpd_req_t *r)
{
    if (!authorized(r))
        return error(r, "403 Forbidden", "Refresh this page and retry.");
    if (device_busy() || portal_updating())
        return error(r, "409 Conflict", "End the conversation and wait for any update to finish.");
    cJSON *j = read_json(r), *forget = cJSON_GetObjectItem(j, "forget");
    bool removing = cJSON_IsString(forget);
    if (!removing) {
        cJSON_Delete(j);
        return error(r, "409 Conflict", "Use CONNECT ANOTHER NETWORK to scan and verify before saving.");
    }
    if (!portal_update_lock()) {
        cJSON_Delete(j);
        return error(r, "409 Conflict", "An update or restart is already in progress.");
    }
    esp_err_t e = wifi_profiles_forget(forget->valuestring);
    cJSON_Delete(j);
    if (e != ESP_OK) {
        portal_update_unlock();
        return error(r, "400 Bad Request", e == ESP_ERR_NO_MEM
                     ? "8 networks saved. Forget one first."
                     : "Could not save Wi-Fi. Check your input.");
    }
    ui_show_network();
    ui_set("WI-FI SAVED", "Restarting device");
    if (xTaskCreate(restart, "restart", 2048, NULL, 4, NULL) != pdPASS) {
        portal_update_unlock();
        ui_set("NETWORK FORGOTTEN", "Restart device manually");
        return error(r, "500 Internal Server Error", "Network forgotten, but restart unavailable. Restart the device manually.");
    }
    return error(r, "200 OK", "Network forgotten. Restarting device.");
}
esp_err_t portal_start(void)
{
    snprintf(nonce, sizeof(nonce), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 6144;
    c.max_uri_handlers = 12;
    c.max_open_sockets = 2;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 10;
    httpd_handle_t server = NULL;
    esp_err_t e = httpd_start(&server, &c);
    if (e != ESP_OK)
        return e;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root},
        {.uri = "/api/wifi/setup", .method = HTTP_POST, .handler = wifi_setup},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = save},
        {.uri = "/api/netcheck", .method = HTTP_POST, .handler = netcheck},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi}};
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) {
            httpd_stop(server);
            return e;
        }
    }
    return ESP_OK;
}
