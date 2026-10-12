#include "wifi.h"
#include "wifi_setup.h"
#include "wifi_profiles.h"
#include "cJSON.h"
#include <stdlib.h>

#include "ui.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include <stdatomic.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define WIFI_CONNECTED BIT0
#define WIFI_DISCONNECTED BIT1
#define WIFI_EXHAUSTED BIT2
#define WIFI_RETRY BIT3
#define CONFIG_NAMESPACE WIFI_CONFIG_NAMESPACE

static const char *TAG = "passport_wifi";
static EventGroupHandle_t s_wifi_events;
static atomic_bool provisioning, radio_busy, setup_claimed;
static atomic_uint disconnect_reason;
static char setup_ssid[33];
static atomic_bool setup_website;
static wifi_profiles_t saved_profiles;
bool voice_wifi_connected(void)
{
    return s_wifi_events && (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED);
}
bool voice_wifi_provisioning(void)
{
    return atomic_load(&provisioning);
}
void voice_wifi_setup_next(void)
{
    setup_website = !setup_website;
    ui_show_setup(setup_ssid, setup_website);
}
esp_err_t voice_wifi_request_setup(void)
{
    nvs_handle_t n;
    esp_err_t e = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &n);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u8(n, "setup_once", 1);
    if (e == ESP_OK)
        e = nvs_commit(n);
    nvs_close(n);
    if (e == ESP_OK)
        esp_restart();
    return e;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
        atomic_store(&disconnect_reason, event->reason);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED);
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP acquired");
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED);
    }
}

esp_err_t voice_wifi_save(const char *ssid, const char *password)
{
    return wifi_profiles_save(ssid, password);
}

bool voice_wifi_radio_take(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&radio_busy, &expected, true);
}
void voice_wifi_radio_give(void) { atomic_store(&radio_busy, false); }
void voice_wifi_setup_claim(void) { atomic_store(&setup_claimed, true); }

// Called only while the setup worker owns the radio. The AP remains available.
esp_err_t voice_wifi_test(const char *ssid, const char *password, unsigned *reason)
{
    xEventGroupClearBits(s_wifi_events, WIFI_DISCONNECTED);
    if (esp_wifi_disconnect() == ESP_OK)
        xEventGroupWaitBits(s_wifi_events, WIFI_DISCONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED | WIFI_DISCONNECTED);
    atomic_store(&disconnect_reason, 0);
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (e == ESP_OK) e = esp_wifi_connect();
    EventBits_t bits = e == ESP_OK ? xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED | WIFI_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000)) : 0;
    *reason = atomic_load(&disconnect_reason);
    return bits & WIFI_CONNECTED ? ESP_OK : ESP_FAIL;
}

// A single task owns station connection attempts. Event callbacks only report state;
// authentication/DHCP failures cannot trap us retrying one unavailable network forever.
static void connection_task(void *unused)
{
    (void)unused;
    unsigned index = 0, attempts = 0;
    bool boot_search = true;
    for (;;) {
        if (voice_wifi_provisioning()) {
            if (atomic_load(&setup_claimed)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
            wifi_sta_list_t clients = {0};
            // Scanning/roaming can change the shared radio channel. Let a phone
            // finish using the setup page before resuming background attempts.
            if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (!voice_wifi_radio_take()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (voice_wifi_provisioning() && atomic_load(&setup_claimed)) {
            voice_wifi_radio_give();
            continue;
        }
        wifi_profile_t *p = &saved_profiles.entries[index];
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, p->ssid, strlen(p->ssid));
        memcpy(config.sta.password, p->password, strlen(p->password));
        config.sta.threshold.authmode = p->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED | WIFI_DISCONNECTED);
        if (!voice_wifi_provisioning())
            ui_set("CONNECTING WI-FI", "Trying saved networks\nHold OK: home");
        ESP_LOGI(TAG, "Trying saved network %u/%u", index + 1, saved_profiles.count);
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (e == ESP_OK)
            e = esp_wifi_connect();
        EventBits_t bits = e == ESP_OK
                              ? xEventGroupWaitBits(s_wifi_events,
                                                    WIFI_CONNECTED | WIFI_DISCONNECTED,
                                                    pdFALSE, pdFALSE, pdMS_TO_TICKS(10000))
                              : 0;
        if (bits & WIFI_CONNECTED) {
            boot_search = false;
            // In fallback AP mode main has not started the voice service. Restart into
            // the normal boot path once a saved hotspot becomes available again.
            if (voice_wifi_provisioning()) {
                wifi_sta_list_t clients = {0};
                esp_wifi_ap_get_sta_list(&clients);
                if (atomic_load(&setup_claimed) || clients.num > 0) {
                    voice_wifi_radio_give();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                ui_set("WI-FI CONNECTED", "Restarting device");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            voice_wifi_radio_give();
            xEventGroupWaitBits(s_wifi_events, WIFI_DISCONNECTED, pdFALSE, pdTRUE,
                                portMAX_DELAY);
            // Give a previously working network one fresh attempt before moving on.
            continue;
        }
        // Finish a pending association/DHCP attempt before changing station config.
        // Waiting for its disconnect event also drains the previous attempt's events.
        if (!(bits & WIFI_DISCONNECTED) && esp_wifi_disconnect() == ESP_OK)
            xEventGroupWaitBits(s_wifi_events, WIFI_DISCONNECTED, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(1000));
        voice_wifi_radio_give();
        index = (index + 1) % saved_profiles.count;
        if (boot_search && ++attempts == saved_profiles.count) {
            xEventGroupSetBits(s_wifi_events, WIFI_EXHAUSTED);
            // Let the boot task finish configuring AP+STA before continuing retries.
            xEventGroupWaitBits(s_wifi_events, WIFI_RETRY, pdTRUE, pdTRUE, portMAX_DELAY);
            boot_search = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
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

static bool url_decode(char *output, size_t size, const char *input, size_t length)
{
    size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        if (written + 1 >= size)
            return false;
        char c = input[i];
        if (c == '+')
            c = ' ';
        else if (c == '%') {
            if (i + 2 >= length)
                return false;
            int hi = hex_value(input[i + 1]), lo = hex_value(input[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (!c)
            return false;
        output[written++] = c;
    }
    output[written] = 0;
    return true;
}

static bool form_value(const char *body, const char *name, char *value, size_t value_size)
{
    size_t name_len = strlen(name);
    const char *cursor = body;
    while (cursor && *cursor) {
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            const char *start = cursor + name_len + 1;
            const char *end = strchr(start, '&');
            return url_decode(value, value_size, start,
                              end ? (size_t)(end - start) : strlen(start));
        }
        cursor = strchr(cursor, '&');
        if (cursor)
            cursor++;
    }
    return false;
}

static esp_err_t saved_networks(httpd_req_t *request)
{
    wifi_profiles_t profiles;
    if (wifi_profiles_load(&profiles) != ESP_OK)
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not read saved networks");
    cJSON *networks = cJSON_CreateArray();
    if (!networks)
        return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < profiles.count; ++i)
        cJSON_AddItemToArray(networks, cJSON_CreateString(profiles.entries[i].ssid));
    memset(&profiles, 0, sizeof(profiles));
    char *body = cJSON_PrintUnformatted(networks);
    cJSON_Delete(networks);
    if (!body)
        return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(request, body);
    free(body);
    return e;
}

static esp_err_t setup_page(httpd_req_t *request)
{
    extern const char setup_start[] asm("_binary_setup_html_start");
    extern const char setup_end[] asm("_binary_setup_html_end");
    // Canonical origin makes captive-portal probes and relative API URLs agree.
    char host[80] = {0};
    httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host));
    if (strcmp(host, "192.168.4.1") && strcmp(host, "192.168.4.1:80")) {
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
        return httpd_resp_sendstr(request, "Open http://192.168.4.1/ after joining the device hotspot.");
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, setup_start, setup_end - setup_start - 1);
}

static void delayed_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static esp_err_t save_config(httpd_req_t *request)
{
    if (!wifi_setup_authorized(request))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Reload this page.");
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
    esp_err_t e;
    bool forget = form_value(body, "forget", ssid, sizeof(ssid));
    if (forget) {
        voice_wifi_setup_claim();
        if (!voice_wifi_radio_take()) {
            httpd_resp_set_status(request, "409 Conflict");
            return httpd_resp_sendstr(request, "Wait for Wi-Fi work to finish.");
        }
        e = wifi_profiles_forget(ssid);
    } else {
        if (!form_value(body, "ssid", ssid, sizeof(ssid)) || !ssid[0])
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID required");
        if (!form_value(body, "password", password, sizeof(password)))
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid password");
        esp_err_t result = wifi_setup_connect(request, ssid, password);
        memset(password, 0, sizeof(password));
        memset(body, 0, sizeof(body));
        return result;
    }
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));
    if (e != ESP_OK) {
        voice_wifi_radio_give();
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   e == ESP_ERR_NO_MEM ? "8 networks saved. Forget one first."
                                                       : "Could not save Wi-Fi. Check your input.");
    }

    ui_set("WI-FI SAVED", "Restarting device");
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_sendstr(request, forget ? "Network forgotten. Restarting device."
                                      : "Wi-Fi saved; other networks kept. Restarting. If this is your phone hotspot, enable it now.");
    if (xTaskCreate(delayed_restart, "restart", 2048, NULL, 4, NULL) != pdPASS) {
        voice_wifi_radio_give();
        ui_set("NETWORK FORGOTTEN", "Restart device manually");
    }
    return ESP_OK;
}

static void dns_task(void *unused)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        if (fd >= 0)
            close(fd);
        ESP_LOGW(TAG, "captive DNS unavailable; use setup QR/address");
        vTaskDelete(NULL);
        return;
    }
    uint8_t packet[512];
    for (;;) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int n = recvfrom(fd, packet, sizeof(packet) - 16, 0, (struct sockaddr *)&client, &len);
        if (n < 17 || (packet[2] & 0x80) || packet[4] || packet[5] != 1)
            continue;
        size_t pos = 12;
        while (pos < n && packet[pos] && packet[pos] <= 63)
            pos += packet[pos] + 1;
        if (pos + 5 > n || packet[pos] != 0)
            continue;
        pos++;
        bool a = packet[pos] == 0 && packet[pos + 1] == 1 && packet[pos + 2] == 0 &&
                 packet[pos + 3] == 1;
        pos += 4;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = a ? 1 : 0;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;
        if (a) {
            const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(packet + pos, answer, sizeof(answer));
            pos += sizeof(answer);
        }
        sendto(fd, packet, pos, 0, (struct sockaddr *)&client, len);
    }
}

static void setup_screen_task(void *unused)
{
    bool joined = false;
    for (;;) {
        wifi_sta_list_t clients = {0};
        if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0 && !joined) {
            joined = true;
            setup_website = true;
            ui_show_setup(setup_ssid, true);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static esp_err_t start_provisioning(bool station_running)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(setup_ssid, sizeof(setup_ssid), "mic-on-%02X%02X", mac[4], mac[5]);

    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!station_running) {
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    }
    wifi_config_t config = {0};
    config.ap.ssid_len = strlen(setup_ssid);
    memcpy(config.ap.ssid, setup_ssid, config.ap.ssid_len);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    if (!station_running)
        ESP_ERROR_CHECK(esp_wifi_start());

    // Advertise a local DNS resolver so phones can open their captive-portal assistant.
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns.ip.u_addr.ip4, 192, 168, 4, 1);
    // The default AP event may have started DHCP asynchronously. DNS assist is optional;
    // never reboot an otherwise usable QR provisioning AP on an already-stopped/started result.
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(ap));
    uint8_t offer_dns = 2; // lwIP dhcps OFFER_DNS
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
        ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(ap));
    ESP_ERROR_CHECK(xTaskCreate(dns_task, "setup_dns", 3072, NULL, 2, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    server_config.max_uri_handlers = 12;
    server_config.max_open_sockets = 3;
    server_config.stack_size = 6144;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 5;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));
    const httpd_uri_t root = {.uri = "/*", .method = HTTP_GET, .handler = setup_page};
    const httpd_uri_t configure = {
        .uri = "/configure", .method = HTTP_POST, .handler = save_config};
    const httpd_uri_t networks = {.uri = "/networks", .method = HTTP_GET, .handler = saved_networks};
    wifi_setup_register(server);
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &networks));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &configure));

    atomic_store(&provisioning, true);
    setup_website = false;
    ui_show_setup(setup_ssid, false);
    ESP_ERROR_CHECK(xTaskCreate(setup_screen_task, "setup_screen", 3072, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "provisioning AP %s", setup_ssid);
    return ESP_ERR_NOT_FINISHED;
}

esp_err_t voice_wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_events = xEventGroupCreate();

    nvs_handle_t nvs;
    if (nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        uint8_t once = 0;
        nvs_get_u8(nvs, "setup_once", &once);
        if (once) {
            nvs_erase_key(nvs, "setup_once");
            nvs_commit(nvs);
        }
        nvs_close(nvs);
        if (once)
            return start_provisioning(false);
    }
    esp_err_t loaded = wifi_profiles_load(&saved_profiles);
    if (loaded != ESP_OK || !saved_profiles.count) {
        if (loaded != ESP_OK)
            ESP_LOGE(TAG, "Could not read saved networks: %s", esp_err_to_name(loaded));
        return start_provisioning(false);
    }

    ui_set("CONNECTING WI-FI", "Trying saved networks\nSetup on failure");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // Credentials belong to our atomic profile blob, not the driver's single-network NVS.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(xTaskCreate(connection_task, "wifi_connect", 4096, NULL, 3, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED | WIFI_EXHAUSTED,
                                         pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED)
        return ESP_OK;
    ESP_LOGW(TAG, "Saved networks unavailable; opening setup while retrying");
    esp_err_t result = start_provisioning(true);
    xEventGroupSetBits(s_wifi_events, WIFI_RETRY);
    return result;
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
