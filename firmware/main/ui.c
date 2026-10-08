#include "ui.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_check.h"

static lv_obj_t *state_label, *detail_label, *level_bar;

void ui_init(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    assert(bsp_lvgl_init());
    bsp_display_backlight(65);
    bsp_lvgl_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xf0f4f5), 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "GPT LIVE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 24);
    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "AI Passport / personal voice");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x94a5ae), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 18, 56);
    state_label = lv_label_create(screen);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(state_label, lv_color_hex(0x80ded0), 0);
    lv_obj_align(state_label, LV_ALIGN_TOP_LEFT, 18, 110);
    detail_label = lv_label_create(screen);
    lv_obj_set_width(detail_label, 204);
    lv_obj_align(detail_label, LV_ALIGN_TOP_LEFT, 18, 150);
    level_bar = lv_bar_create(screen);
    lv_obj_set_size(level_bar, 204, 7);
    lv_obj_align(level_bar, LV_ALIGN_TOP_LEFT, 18, 223);
    lv_obj_set_style_bg_color(level_bar, lv_color_hex(0x80ded0), LV_PART_INDICATOR);
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "OK: connect / hold to talk\nDOWN: end conversation\nUP: volume");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94a5ae), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 18, -18);
    bsp_lvgl_unlock();
    ui_set("STARTING", "Preparing audio...");
}

void ui_set(const char *state, const char *detail)
{
    if (!state_label || !bsp_lvgl_lock(100)) return;
    lv_label_set_text(state_label, state);
    lv_label_set_text(detail_label, detail);
    bsp_lvgl_unlock();
}

void ui_level(int level)
{
    if (!level_bar || !bsp_lvgl_lock(10)) return;
    lv_bar_set_value(level_bar, level > 100 ? 100 : level, LV_ANIM_OFF);
    bsp_lvgl_unlock();
}
