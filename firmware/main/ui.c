#include "ui.h"
#include "settings.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Only built-in Latin fonts. No captions, scrolling labels or level animation.
static lv_obj_t *status_label, *detail_label, *battery_label, *volume_label, *guide, *qr,
    *volume_popup;
static int64_t last_touch, volume_until;
static bool screen_on = true, setup_visible, menu_visible;
static bool live_mode;
static int shown_battery = -2, shown_volume = -1;

static lv_obj_t *label(int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(lv_screen_active());
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
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
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_14, 0);
    lv_obj_t *title = label(14, 12, 212, 26, 0xf0f4f5);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "GPT LIVE");
    battery_label = label(14, 44, 112, 24, 0xf0f4f5);
    volume_label = label(138, 44, 88, 24, 0xf0f4f5);
    lv_label_set_text(battery_label, "BAT: --");
    lv_label_set_text(volume_label, "VOL: --");
    status_label = label(14, 77, 212, 24, 0x80ded0);
    guide = label(14, 114, 212, 128, 0xf0f4f5);
    lv_label_set_text(guide,
                      "Hold OK: talk\nRelease OK: send\nUP/DOWN: volume\nDOWN+OK 0.8s: menu");
    detail_label = label(14, 262, 212, 50, 0xa4b5be);
    volume_popup = label(38, 174, 180, 32, 0x80ded0);
    lv_obj_set_style_bg_color(volume_popup, lv_color_hex(0x111c22), 0);
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_COVER, 0);
    lv_obj_set_hidden(volume_popup, true);
    last_touch = esp_timer_get_time();
    bsp_lvgl_unlock();
    ui_battery(-1, settings_get().volume);
    ui_set("STARTING", "Preparing audio and Wi-Fi");
}
void ui_set(const char *state, const char *detail)
{
    if (!status_label || !bsp_lvgl_lock(100))
        return;
    // Keep a scanned setup page stable while background network events arrive.
    if (!qr && !menu_visible) {
        if (strcmp(lv_label_get_text(status_label), state))
            lv_label_set_text(status_label, state);
        if (strcmp(lv_label_get_text(detail_label), detail))
            lv_label_set_text(detail_label, detail);
    }
    bsp_lvgl_unlock();
}
void ui_battery(int percent, int volume)
{
    if (!bsp_lvgl_lock(100))
        return;
    if (percent != shown_battery) {
        if (percent >= 0 && percent <= 100)
            lv_label_set_text_fmt(battery_label, "BAT: %d%%", percent);
        else
            lv_label_set_text(battery_label, "BAT: --");
        shown_battery = percent;
    }
    if (volume != shown_volume) {
        lv_label_set_text_fmt(volume_label, "VOL: %d", volume);
        shown_volume = volume;
    }
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
    if (volume_popup && now >= volume_until)
        lv_obj_set_hidden(volume_popup, true);
    if (active || setup_visible || menu_visible) {
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
static void show_qr(const char *title, const char *payload, const char *instructions, bool setup)
{
    ui_wake();
    if (!bsp_lvgl_lock(100))
        return;
    lv_obj_set_hidden(volume_popup, true);
    setup_visible = setup;
    lv_obj_set_hidden(guide, true);
    lv_label_set_text(status_label, title);
    lv_label_set_text(detail_label, instructions);
    if (!qr) {
        qr = lv_qrcode_create(lv_screen_active());
        lv_qrcode_set_size(qr, 150);
        lv_qrcode_set_quiet_zone(qr, true);
        lv_obj_set_pos(qr, 45, 105);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
    }
    if (lv_qrcode_update(qr, payload, strlen(payload)) != LV_RESULT_OK)
        lv_label_set_text(status_label, "USE ADDRESS BELOW");
    bsp_lvgl_unlock();
}
void ui_show_setup(const char *ssid, bool website)
{
    char payload[96], detail[128];
    if (website) {
        snprintf(detail, sizeof(detail), "http://192.168.4.1\nOK: back  Hold OK: retry");
        show_qr("2. SCAN TO SET UP", "http://192.168.4.1/", detail, true);
    } else {
        snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
        snprintf(detail, sizeof(detail), "%s\nThen OK for setup page", ssid);
        show_qr("1. SCAN TO JOIN WI-FI", payload, detail, true);
    }
}
void ui_show_settings(const char *url)
{
    char detail[112];
    snprintf(detail, sizeof(detail), "%s\nHold OK: back (same Wi-Fi)", url);
    show_qr("SCAN FOR SETTINGS", url, detail, false);
}
void ui_show_guide(void)
{
    if (!bsp_lvgl_lock(100))
        return;
    if (qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    setup_visible = false;
    menu_visible = false;
    lv_obj_set_size(guide, 212, 128);
    lv_label_set_text(guide, live_mode ? "LIVE: speak freely\nTap OK: stop reply\nHold OK: switch "
                                         "to PTT\nUP/DOWN: volume\nDOWN+OK 0.8s: menu"
                                       : "Hold OK: talk\nRelease OK: send\nTap OK: stop "
                                         "reply\nUP/DOWN: volume\nDOWN+OK 0.8s: menu");
    lv_obj_set_hidden(guide, false);
    bsp_lvgl_unlock();
}

void ui_mode(bool continuous)
{
    live_mode = continuous;
}
void ui_show_menu(const char *title, const char *const *items, int count, int selected,
                  const char *hint)
{
    ui_wake();
    if (!bsp_lvgl_lock(100))
        return;
    if (qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    lv_obj_set_hidden(volume_popup, true);
    menu_visible = true;
    setup_visible = false;
    lv_obj_set_hidden(guide, false);
    lv_obj_set_size(guide, 212, 144);
    char text[300] = {0};
    size_t used = 0;
    int first = selected >= 6 ? selected - 5 : 0;
    for (int i = first; i < count && i < first + 6; i++) {
        used += snprintf(text + used, sizeof(text) - used, "%c %s%s", i == selected ? '>' : ' ',
                         items[i], i < count - 1 ? "\n" : "");
    }
    lv_label_set_text(status_label, title);
    lv_label_set_text(guide, text);
    lv_label_set_text(detail_label, hint);
    bsp_lvgl_unlock();
}

void ui_volume(int volume)
{
    if (!bsp_lvgl_lock(100))
        return;
    volume_until = esp_timer_get_time() + 1500000;
    lv_label_set_text_fmt(volume_popup, "  Volume: %d", volume);
    lv_obj_set_hidden(volume_popup, false);
    lv_obj_move_foreground(volume_popup);
    bsp_lvgl_unlock();
}
