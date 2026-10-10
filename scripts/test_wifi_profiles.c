// Compile the production profile store against an in-memory NVS with failed writes.
#include "wifi_profiles.h"
#include "nvs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static wifi_profiles_t disk, pending;
static bool exists, staged, fail_commit, fail_write;
static const char *legacy_ssid[2], *legacy_password[2];

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *n)
{
    (void)mode;
    *n = !strcmp(name, "gpt_live") ? 0 : 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t n) { (void)n; staged = false; }
esp_err_t nvs_get_blob(nvs_handle_t n, const char *key, void *out, size_t *size)
{
    (void)n; assert(!strcmp(key, "wifi_profiles"));
    if (!exists) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= sizeof(disk));
    memcpy(out, &disk, sizeof(disk)); *size = sizeof(disk); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t n, const char *key, const void *in, size_t size)
{
    (void)n; assert(!strcmp(key, "wifi_profiles")); assert(size == sizeof(disk));
    if (fail_write) return ESP_FAIL;
    memcpy(&pending, in, size); staged = true; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t n, const char *key, char *out, size_t *size)
{
    const char *value = !strcmp(key, "ssid") ? legacy_ssid[n] : legacy_password[n];
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    size_t length = strlen(value) + 1;
    if (*size < length) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, value, length); *size = length; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t n)
{
    (void)n;
    if (fail_commit) return ESP_FAIL;
    assert(staged); disk = pending; exists = true; staged = false; return ESP_OK;
}
static wifi_profiles_t snapshot(void)
{
    wifi_profiles_t p;
    assert(wifi_profiles_load(&p) == ESP_OK);
    return p;
}
static void *save_thread(void *name)
{
    assert(wifi_profiles_save(name, "12345678") == ESP_OK);
    return NULL;
}
int main(void)
{
    assert(snapshot().count == 0);
    legacy_ssid[1] = "Old Home"; legacy_password[1] = "old-password";
    wifi_profiles_t p = snapshot();
    assert(p.count == 1 && !strcmp(p.entries[0].ssid, "Old Home"));
    legacy_ssid[0] = "Home"; legacy_password[0] = "home-password";
    p = snapshot(); assert(!strcmp(p.entries[0].ssid, "Home"));
    assert(wifi_profiles_save("Company", "company-password") == ESP_OK);
    assert(wifi_profiles_save("Phone", "phone-password") == ESP_OK);
    p = snapshot(); assert(p.count == 3);
    assert(!strcmp(p.entries[0].ssid, "Phone") && !strcmp(p.entries[2].ssid, "Home"));
    assert(!strcmp(p.entries[2].password, "home-password"));
    assert(wifi_profiles_save("Home", "new-password") == ESP_OK);
    p = snapshot(); assert(p.count == 3 && !strcmp(p.entries[0].ssid, "Home"));
    assert(!strcmp(p.entries[0].password, "new-password"));
    assert(!strcmp(p.entries[1].ssid, "Phone"));
    assert(wifi_profiles_save("bad", "short") == ESP_ERR_INVALID_ARG);
    assert(wifi_profiles_save("", "12345678") == ESP_ERR_INVALID_ARG);
    assert(wifi_profiles_save(NULL, "12345678") == ESP_ERR_INVALID_ARG);
    assert(wifi_profiles_save("123456789012345678901234567890123", "12345678") == ESP_ERR_INVALID_ARG);
    char raw[65]; memset(raw, 'z', 64); raw[64] = 0;
    assert(wifi_profiles_save("bad", raw) == ESP_ERR_INVALID_ARG);
    memset(raw, 'a', 64); assert(wifi_profiles_save("Raw key", raw) == ESP_OK);
    assert(wifi_profiles_save("开放网络", "") == ESP_OK);
    fail_commit = true;
    assert(wifi_profiles_save("Lost", "12345678") == ESP_FAIL);
    fail_commit = false;
    assert(snapshot().count == 5);
    fail_write = true;
    assert(wifi_profiles_save("Home", "changed-password") == ESP_FAIL);
    fail_write = false;
    p = snapshot(); assert(!strcmp(p.entries[2].password, "new-password"));
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, save_thread, "Concurrent A"));
    assert(!pthread_create(&b, NULL, save_thread, "Concurrent B"));
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(snapshot().count == 7);
    assert(wifi_profiles_save("Last", "12345678") == ESP_OK);
    assert(wifi_profiles_save("Overflow", "12345678") == ESP_ERR_NO_MEM);
    assert(snapshot().count == 8);
    assert(wifi_profiles_save("Home", "full-update") == ESP_OK);
    p = snapshot(); assert(p.count == 8 && !strcmp(p.entries[0].password, "full-update"));
    assert(wifi_profiles_forget("Unknown") == ESP_ERR_NOT_FOUND);
    assert(wifi_profiles_forget("Phone") == ESP_OK);
    assert(snapshot().count == 7);
    while ((p = snapshot()).count)
        assert(wifi_profiles_forget(p.entries[0].ssid) == ESP_OK);
    // Legacy keys still exist, but an explicitly empty list remains empty on reboot.
    assert(snapshot().count == 0);
    disk.count = 9;
    assert(wifi_profiles_load(&p) == ESP_ERR_INVALID_STATE);
    disk.count = 1; memset(disk.entries[0].ssid, 'x', 33);
    assert(wifi_profiles_load(&p) == ESP_ERR_INVALID_STATE);
    disk.version = 2;
    assert(wifi_profiles_load(&p) == ESP_ERR_INVALID_STATE);
    puts("PASS: legacy migration, retained networks, updates, UTF-8/open/raw keys, failed writes, concurrent saves, capacity, forget, empty reboot, corrupt blob");
}
