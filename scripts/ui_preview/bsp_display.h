#pragma once
#include "lvgl.h"
#include "esp_err.h"
esp_err_t bsp_display_init(void);
lv_display_t *bsp_lvgl_init(void);
void bsp_display_backlight(uint8_t percent);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
