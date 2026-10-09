#include "settings.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdbool.h>
static settings_t prefs = {.volume = 70, .brightness = 65, .screen_seconds = 60,
                          .idle_seconds = 180, .duplex = 1};
static const char *voices[VOICE_COUNT] = {"cove", "juniper", "maple", "spruce", "ember",
                                          "vale", "breeze",  "arbor", "sol"};
const char *settings_voice(int i)
{
    return voices[i >= 0 && i < VOICE_COUNT ? i : 0];
}
static SemaphoreHandle_t lock;
static bool valid(settings_t p)
{
    return p.volume >= 0 && p.volume <= 100 && p.brightness >= 5 && p.brightness <= 100 &&
           p.screen_seconds >= 15 && p.screen_seconds <= 600 && p.idle_seconds >= 30 &&
           p.idle_seconds <= 900 && p.mode >= 0 && p.mode <= 1 && p.voice >= 0 &&
           p.voice < VOICE_COUNT && p.duplex >= 0 && p.duplex <= 1;
}
void settings_init(void)
{
    lock = xSemaphoreCreateMutex();
    assert(lock);
    nvs_handle_t n;
    if (nvs_open("gpt_live", NVS_READWRITE, &n) != ESP_OK)
        return;
    settings_t saved = prefs;
    size_t size = sizeof(saved);
    bool loaded = nvs_get_blob(n, "prefs_v4", &saved, &size) == ESP_OK && size == sizeof(saved) &&
                  valid(saved);
    if (!loaded) {
        // Read the exact old layout; retain Wi-Fi and all existing device preferences.
        int old[4];
        size = sizeof(old);
        bool old_v3 = nvs_get_blob(n, "prefs_v3", old, &size) == ESP_OK && size == sizeof(old);
        if (!old_v3) {
            size = sizeof(old);
            loaded = nvs_get_blob(n, "prefs_v2", old, &size) == ESP_OK && size == sizeof(old);
        }
        if (old_v3 || loaded) {
            saved = prefs;
            saved.volume = old[0];
            saved.brightness = old[1];
            saved.screen_seconds = old[2];
            saved.idle_seconds = old[3];
            if (!old_v3 && saved.volume > 0 && saved.volume < 70)
                saved.volume = 70;
            loaded = valid(saved);
        }
        if (loaded) {
            nvs_set_blob(n, "prefs_v4", &saved, sizeof(saved));
            nvs_commit(n);
        }
    }
    if (loaded)
        prefs = saved;
    nvs_close(n);
    prefs.reserved_style[0] = prefs.reserved_style[1] = prefs.reserved_style[2] = 0;
    prefs.duplex = 1;
}
settings_t settings_get(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    settings_t value = prefs;
    xSemaphoreGive(lock);
    return value;
}
esp_err_t settings_save(settings_t p)
{
    p.duplex = 1; // Retired NVS field; live is always full duplex.
    p.reserved_style[0] = p.reserved_style[1] = p.reserved_style[2] = 0;
    if (!valid(p))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    nvs_handle_t n;
    esp_err_t err = nvs_open("gpt_live", NVS_READWRITE, &n);
    if (err == ESP_OK) {
        err = nvs_set_blob(n, "prefs_v4", &p, sizeof(p));
        if (err == ESP_OK)
            err = nvs_commit(n);
        nvs_close(n);
    }
    if (err == ESP_OK)
        prefs = p;
    xSemaphoreGive(lock);
    return err;
}

void settings_adjust_volume(int delta)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    prefs.volume += delta;
    if (prefs.volume < 0)
        prefs.volume = 0;
    if (prefs.volume > 100)
        prefs.volume = 100;
    xSemaphoreGive(lock);
}
