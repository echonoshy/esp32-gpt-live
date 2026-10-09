#include "settings.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdbool.h>
static settings_t prefs = {55, 65, 60, 180};
static SemaphoreHandle_t lock;
static bool valid(settings_t p)
{
    return p.volume >= 0 && p.volume <= 90 && p.brightness >= 5 && p.brightness <= 100 &&
           p.screen_seconds >= 15 && p.screen_seconds <= 600 && p.idle_seconds >= 30 &&
           p.idle_seconds <= 900;
}
void settings_init(void)
{
    lock = xSemaphoreCreateMutex();
    assert(lock);
    nvs_handle_t n;
    if (nvs_open("gpt_live", NVS_READONLY, &n) != ESP_OK)
        return;
    settings_t saved;
    size_t size = sizeof(saved);
    if (nvs_get_blob(n, "prefs_v2", &saved, &size) == ESP_OK && size == sizeof(saved) &&
        valid(saved))
        prefs = saved;
    nvs_close(n);
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
    if (!valid(p))
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    nvs_handle_t n;
    esp_err_t err = nvs_open("gpt_live", NVS_READWRITE, &n);
    if (err == ESP_OK) {
        err = nvs_set_blob(n, "prefs_v2", &p, sizeof(p));
        if (err == ESP_OK)
            err = nvs_commit(n);
        nvs_close(n);
    }
    if (err == ESP_OK)
        prefs = p;
    xSemaphoreGive(lock);
    return err;
}
