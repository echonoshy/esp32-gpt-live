#include "wifi_profiles.h"
#include "nvs.h"
#include <string.h>
#include <stdbool.h>
#include <sys/lock.h>

// USB and HTTP can save concurrently. Serialize the complete read/modify/commit.
static _lock_t profiles_lock;

static bool valid_credentials(const char *ssid, const char *password)
{
    size_t n = strlen(password);
    if (!ssid[0] || strlen(ssid) > 32 || n > 64 || (n && n < 8))
        return false;
    if (n == 64) {
        for (size_t i = 0; i < n; ++i)
            if (!((password[i] >= '0' && password[i] <= '9') ||
                  (password[i] >= 'a' && password[i] <= 'f') ||
                  (password[i] >= 'A' && password[i] <= 'F')))
                return false;
    }
    return true;
}

static esp_err_t load(wifi_profiles_t *profiles)
{
    memset(profiles, 0, sizeof(*profiles));
    profiles->version = 1;
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READONLY, &nvs);
    if (e == ESP_OK) {
        size_t size = sizeof(*profiles);
        e = nvs_get_blob(nvs, "wifi_profiles", profiles, &size);
        nvs_close(nvs);
        if (e == ESP_OK) {
            if (size != sizeof(*profiles) || profiles->version != 1 ||
                profiles->count > WIFI_PROFILE_MAX)
                return ESP_ERR_INVALID_STATE;
            for (unsigned i = 0; i < profiles->count; ++i) {
                wifi_profile_t *p = &profiles->entries[i];
                if (!memchr(p->ssid, 0, sizeof(p->ssid)) ||
                    !memchr(p->password, 0, sizeof(p->password)) ||
                    !valid_credentials(p->ssid, p->password))
                    return ESP_ERR_INVALID_STATE;
            }
            // An explicitly empty list must not resurrect forgotten legacy credentials.
            return ESP_OK;
        }
    }
    if (e != ESP_ERR_NVS_NOT_FOUND)
        return e;

    // Lazy migration: keep the original keys for older firmware until a list is saved.
    const char *namespaces[] = {WIFI_CONFIG_NAMESPACE, "ripple"};
    for (unsigned i = 0; i < 2; ++i) {
        e = nvs_open(namespaces[i], NVS_READONLY, &nvs);
        if (e == ESP_ERR_NVS_NOT_FOUND)
            continue;
        if (e != ESP_OK)
            return e;
        wifi_profile_t *p = &profiles->entries[0];
        memset(p, 0, sizeof(*p));
        size_t size = sizeof(p->ssid);
        e = nvs_get_str(nvs, "ssid", p->ssid, &size);
        if (e == ESP_OK && p->ssid[0]) {
            size = sizeof(p->password);
            e = nvs_get_str(nvs, "password", p->password, &size);
            if (e == ESP_ERR_NVS_NOT_FOUND)
                e = ESP_OK;
            nvs_close(nvs);
            if (e != ESP_OK)
                return e;
            if (!valid_credentials(p->ssid, p->password))
                return ESP_ERR_INVALID_STATE;
            profiles->count = 1;
            return ESP_OK;
        }
        nvs_close(nvs);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
            return e;
    }
    return ESP_OK;
}

esp_err_t wifi_profiles_load(wifi_profiles_t *profiles)
{
    _lock_acquire(&profiles_lock);
    esp_err_t e = load(profiles);
    _lock_release(&profiles_lock);
    return e;
}

static esp_err_t persist(const wifi_profiles_t *profiles)
{
    nvs_handle_t nvs;
    esp_err_t e = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (e != ESP_OK)
        return e;
    // One blob prevents pairing an old SSID with a new password after a failed write.
    e = nvs_set_blob(nvs, "wifi_profiles", profiles, sizeof(*profiles));
    if (e == ESP_OK)
        e = nvs_commit(nvs);
    nvs_close(nvs);
    return e;
}

esp_err_t wifi_profiles_save(const char *ssid, const char *password)
{
    if (!ssid || !password || !valid_credentials(ssid, password))
        return ESP_ERR_INVALID_ARG;
    _lock_acquire(&profiles_lock);
    wifi_profiles_t profiles;
    esp_err_t e = load(&profiles);
    if (e == ESP_OK) {
        unsigned index = 0;
        while (index < profiles.count && strcmp(profiles.entries[index].ssid, ssid))
            ++index;
        if (index == profiles.count && profiles.count == WIFI_PROFILE_MAX) {
            e = ESP_ERR_NO_MEM;
        } else {
            if (index == profiles.count)
                ++profiles.count;
            memmove(&profiles.entries[1], &profiles.entries[0], index * sizeof(wifi_profile_t));
            memset(&profiles.entries[0], 0, sizeof(wifi_profile_t));
            strcpy(profiles.entries[0].ssid, ssid);
            strcpy(profiles.entries[0].password, password);
            e = persist(&profiles);
        }
    }
    memset(&profiles, 0, sizeof(profiles));
    _lock_release(&profiles_lock);
    return e;
}

esp_err_t wifi_profiles_forget(const char *ssid)
{
    if (!ssid || !ssid[0])
        return ESP_ERR_INVALID_ARG;
    _lock_acquire(&profiles_lock);
    wifi_profiles_t profiles;
    esp_err_t e = load(&profiles);
    if (e == ESP_OK) {
        unsigned index = 0;
        while (index < profiles.count && strcmp(profiles.entries[index].ssid, ssid))
            ++index;
        if (index == profiles.count) {
            e = ESP_ERR_NOT_FOUND;
        } else {
            --profiles.count;
            memmove(&profiles.entries[index], &profiles.entries[index + 1],
                    (profiles.count - index) * sizeof(wifi_profile_t));
            memset(&profiles.entries[profiles.count], 0, sizeof(wifi_profile_t));
            e = persist(&profiles);
        }
    }
    memset(&profiles, 0, sizeof(profiles));
    _lock_release(&profiles_lock);
    return e;
}
