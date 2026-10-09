#include "ui.h"
#include "settings.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_timer.h"
#include <stdlib.h>
LV_FONT_DECLARE(passport_cjk_16);
static lv_obj_t *state_label, *detail_label, *level_bar, *battery, *user_label, *reply_label;
static int64_t last_touch;
static bool screen_on = true;
static lv_obj_t *label(lv_obj_t *screen, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(screen);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL);
    lv_label_set_text(obj, "");
    return obj;
}
void ui_init(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    if (!bsp_lvgl_init())
        abort();
    bsp_display_backlight(settings_get().brightness);
    bsp_lvgl_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_font(screen, &passport_cjk_16, 0);
    lv_obj_t *title = label(screen, 14, 14, 126, 22, 0xf0f4f5);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "GPT LIVE");
    battery = label(screen, 146, 17, 90, 20, 0x94a5ae);
    state_label = label(screen, 14, 48, 212, 23, 0x80ded0);
    detail_label = label(screen, 14, 78, 212, 38, 0xa4b5be);
    user_label = label(screen, 14, 125, 212, 38, 0xa4b5be);
    reply_label = label(screen, 14, 171, 212, 92, 0xf0f4f5);
    level_bar = lv_bar_create(screen);
    lv_obj_set_size(level_bar, 212, 5);
    lv_obj_set_pos(level_bar, 14, 274);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x80ded0), LV_PART_INDICATOR);
    lv_obj_t *hint = label(screen, 14, 290, 212, 20, 0x94a5ae);
    lv_label_set_text(hint, "OK 对话/打断  ↓结束 ↑音量");
    last_touch = esp_timer_get_time();
    bsp_lvgl_unlock();
    ui_set("正在启动", "正在准备声音和网络");
}
void ui_set(const char *state, const char *detail)
{
    if (!state_label || !bsp_lvgl_lock(100))
        return;
    lv_label_set_text(state_label, state);
    lv_label_set_text(detail_label, detail);
    bsp_lvgl_unlock();
}
void ui_caption(bool user, const char *text)
{
    if (!bsp_lvgl_lock(100))
        return;
    lv_label_set_text_fmt(user ? user_label : reply_label, "%s%s", user ? "你：" : "", text);
    bsp_lvgl_unlock();
}
void ui_clear_captions(void)
{
    if (!bsp_lvgl_lock(100))
        return;
    lv_label_set_text(user_label, "");
    lv_label_set_text(reply_label, "");
    bsp_lvgl_unlock();
}
void ui_level(int level)
{
    if (!level_bar || !bsp_lvgl_lock(10))
        return;
    lv_bar_set_value(level_bar, level > 100 ? 100 : level, LV_ANIM_OFF);
    bsp_lvgl_unlock();
}
void ui_battery(int percent, int volume)
{
    if (!bsp_lvgl_lock(100))
        return;
    if (percent >= 0)
        lv_label_set_text_fmt(battery, "%d%% · %d", percent, volume);
    else
        lv_label_set_text_fmt(battery, "-- · %d", volume);
    bsp_lvgl_unlock();
}
void ui_wake(void)
{
    if (!bsp_lvgl_lock(100))
        return;
    last_touch = esp_timer_get_time();
    screen_on = true;
    bsp_display_backlight(settings_get().brightness);
    bsp_lvgl_unlock();
}
void ui_tick(bool active)
{
    if (!bsp_lvgl_lock(10))
        return;
    int64_t now = esp_timer_get_time();
    if (active) {
        last_touch = now;
        if (!screen_on) {
            screen_on = true;
            bsp_display_backlight(settings_get().brightness);
        }
    } else if (screen_on && now - last_touch > (int64_t)settings_get().screen_seconds * 1000000) {
        bsp_display_backlight(0);
        screen_on = false;
    }
    bsp_lvgl_unlock();
}
