#include "wifi.h"

#include "ui.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WIFI_CONNECTED BIT0
#define CONFIG_NAMESPACE "gpt_live"

static const char *TAG = "passport_wifi";
static EventGroupHandle_t s_wifi_events;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED);
        ui_set("网络已断开", "正在重连 Wi-Fi");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP acquired");
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED);
    }
}

esp_err_t voice_wifi_save(const char *ssid, const char *password)
{
    if (!ssid || !password || strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (result != ESP_OK)
        return result;
    result = nvs_set_str(nvs, "ssid", ssid);
    if (result == ESP_OK)
        result = nvs_set_str(nvs, "password", password);
    if (result == ESP_OK)
        result = nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

static bool load_string(nvs_handle_t nvs, const char *key, char *value, size_t size)
{
    size_t needed = size;
    return nvs_get_str(nvs, key, value, &needed) == ESP_OK && value[0] != '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static void url_decode(char *output, size_t output_size, const char *input, size_t input_len)
{
    size_t written = 0;
    for (size_t i = 0; i < input_len && written + 1 < output_size; ++i) {
        if (input[i] == '+') {
            output[written++] = ' ';
        } else if (input[i] == '%' && i + 2 < input_len) {
            int hi = hex_value(input[i + 1]);
            int lo = hex_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output[written++] = (char)((hi << 4) | lo);
                i += 2;
            }
        } else {
            output[written++] = input[i];
        }
    }
    output[written] = '\0';
}

static bool form_value(const char *body, const char *name, char *value, size_t value_size)
{
    size_t name_len = strlen(name);
    const char *cursor = body;
    while (cursor && *cursor) {
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            const char *start = cursor + name_len + 1;
            const char *end = strchr(start, '&');
            url_decode(value, value_size, start, end ? (size_t)(end - start) : strlen(start));
            return value[0] != '\0';
        }
        cursor = strchr(cursor, '&');
        if (cursor)
            cursor++;
    }
    return false;
}

static esp_err_t setup_page(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        "<style>body{font:16px sans-serif;max-width:420px;margin:40px auto;padding:20px}"
        "input,button{box-sizing:border-box;width:100%;padding:12px;margin:7px 0}</style>"
        "<h1>GPT Live Passport</h1><p>为设备连接 Wi-Fi。</p>"
        "<form method=post action=/configure>"
        "<input name=ssid placeholder='Wi-Fi 名称' required>"
        "<input name=password type=password placeholder='Wi-Fi 密码'>"

        "<button>保存并重启</button></form>";
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static void delayed_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t save_config(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 512) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    }
    char body[513] = {0};
    int total = 0;
    while (total < request->content_len) {
        int received = httpd_req_recv(request, body + total, request->content_len - total);
        if (received <= 0)
            return ESP_FAIL;
        total += received;
    }
    body[total] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    if (!form_value(body, "ssid", ssid, sizeof(ssid))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID required");
    }
    (void)form_value(body, "password", password, sizeof(password));

    if (voice_wifi_save(ssid, password) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not save Wi-Fi");

    ui_set("网络已保存", "正在重新启动");
    httpd_resp_sendstr(request, "Saved. The Passport is restarting.");
    xTaskCreate(delayed_restart, "restart", 2048, NULL, 4, NULL);
    return ESP_OK;
}

static esp_err_t start_provisioning(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    char ap_name[33];
    snprintf(ap_name, sizeof(ap_name), "GPT-Passport-%02X%02X", mac[4], mac[5]);

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t config = {0};
    config.ap.ssid_len = strlen(ap_name);
    memcpy(config.ap.ssid, ap_name, config.ap.ssid_len);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = setup_page};
    const httpd_uri_t configure = {
        .uri = "/configure", .method = HTTP_POST, .handler = save_config};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &configure));

    char detail[80];
    snprintf(detail, sizeof(detail), "%s\n打开 192.168.4.1", ap_name);
    ui_set("请先配网", detail);
    ESP_LOGI(TAG, "provisioning AP %s", ap_name);
    return ESP_ERR_NOT_FINISHED;
}

esp_err_t voice_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_events = xEventGroupCreate();

    nvs_handle_t nvs;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK &&
        nvs_open("ripple", NVS_READONLY, &nvs) != ESP_OK) {
        return start_provisioning();
    }
    char ssid[33] = {0};
    char password[65] = {0};
    bool configured = load_string(nvs, "ssid", ssid, sizeof(ssid));
    size_t password_size = sizeof(password);
    if (nvs_get_str(nvs, "password", password, &password_size) != ESP_OK)
        password[0] = '\0';
    nvs_close(nvs);
    if (!configured)
        return start_provisioning();

    ui_set("正在联网", "正在连接 Wi-Fi");
    esp_netif_t *station = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED)
        return ESP_OK;
    ESP_LOGW(TAG, "Saved Wi-Fi unavailable; opening setup hotspot");
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event));
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());
    esp_netif_destroy_default_wifi(station);
    return start_provisioning();
}

void voice_wifi_ip(char *output, size_t size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(output, size, IPSTR, IP2STR(&info.ip));
    else
        snprintf(output, size, "192.168.4.1");
}
