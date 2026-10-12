#include "ui.h"
#include "settings.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

// Flash-resident 4 bpp fonts. Selection changes use a single settled layout.
LV_FONT_DECLARE(ui_font_14);
LV_FONT_DECLARE(ui_font_18);
LV_FONT_DECLARE(ui_font_22);
LV_FONT_DECLARE(ui_font_16);
LV_IMAGE_DECLARE(ui_wordmark);
LV_FONT_DECLARE(ui_font_comic_44);
LV_FONT_DECLARE(ui_font_comic_80);
LV_FONT_DECLARE(ui_font_comic_52);
static lv_obj_t *status_label, *detail_label, *battery_label, *volume_label, *guide, *qr,
    *volume_popup, *mode_label, *selection, *status_surface;
static lv_obj_t *home_surface, *body_surface;
static int home_weights[3] = {1024, 0, 0};
static int home_selected;
static const char *home_titles[] = {"VOICE", "LIVE", "SETTINGS"};
static const char *home_descriptions[] = {"TAP TO RECORD", "HANDS-FREE", "DEVICE CONFIG"};
// Comic ink / paper / magenta, yellow underprint and purple corner accents.
enum {
    UI_BACKGROUND = 0x141414,
    UI_TEXT = 0xf3eedb,
    UI_MUTED = 0xc8c3b3,
    UI_ACCENT = 0xffe900,
    UI_SURFACE = 0x8c52c5,
    UI_SELECTION = 0xf50065,
    UI_WARNING = 0xffac70,
    UI_CARD = 0xf3eedb,
    UI_BORDER = 0xf3eedb,
    UI_FOCUS_BORDER = 0xffe900,
    UI_SELECTED_TEXT = 0xf3eedb,
};
enum { UI_MINT = 0x83e8c3, UI_LILAC = 0xb69aff, UI_ORANGE = 0xffac70 };
static char menu_rows[6][64];
static int menu_rows_count, menu_cursor, menu_first, menu_total, menu_row_height;
static bool menu_voice_picker, menu_root;
static int art_value;
static char art_readout[16];
static int art_kind; // 0: voice, 1: live, 2: list, 3: adjustment, 4: confirmation
static void draw_art(lv_event_t *event);
static int64_t last_touch, volume_until;
static bool screen_on = true, setup_visible, menu_visible, home_visible, live_mode, maintenance_visible;
static int shown_battery = -2, shown_volume = -1;

// A single atomic word publishes level and timestamp together. The 24-bit ms
// clock wraps safely; audio callbacks refresh it continuously while active.
static atomic_uint audio_meter[2];
static int art_level, art_phase;
static int live_heights[9] = {8, 8, 8, 8, 8, 8, 8, 8, 8};

void ui_audio_samples(bool playback, const int16_t *samples, size_t count)
{
    if (!samples || !count)
        return;
    int32_t mean = 0;
    uint32_t magnitude = 0, n = 0;
    for (size_t i = 0; i < count; i += 4) {
        mean += samples[i];
        n++;
    }
    mean /= (int32_t)n;
    for (size_t i = 0; i < count; i += 4)
        magnitude += abs((int32_t)samples[i] - mean);
    magnitude /= n;
    // Small noise gate followed by a saturating response, using integer math.
    uint32_t signal = magnitude > 100 ? magnitude - 100 : 0;
    uint32_t level = signal * 255 / (signal + 1800);
    uint32_t stamp = (uint32_t)(esp_timer_get_time() / 1000) & 0xffffff;
    atomic_store_explicit(&audio_meter[playback ? 1 : 0], (stamp << 8) | level,
                          memory_order_relaxed);
}

static int meter_level(bool playback)
{
    uint32_t packed = atomic_load_explicit(&audio_meter[playback ? 1 : 0],
                                          memory_order_relaxed);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return ((now - (packed >> 8)) & 0xffffff) <= 180 ? packed & 255 : 0;
}

static void animate_art(lv_timer_t *timer)
{
    (void)timer;
    if (!screen_on || menu_visible || home_visible || setup_visible || art_kind > 1 ||
        (qr && !lv_obj_is_hidden(qr)) || lv_obj_is_hidden(body_surface)) {
        art_level = 0;
        art_phase = 0;
        for (int i = 0; i < 9; i++) live_heights[i] = 8;
        return;
    }
    const char *state = lv_label_get_text(status_label);
    bool speaking = !strcmp(state, "SPEAKING");
    bool listening = !strcmp(state, "RECORDING") || !strcmp(state, "LISTENING");
    int target = speaking ? meter_level(true) : listening ? meter_level(false) : 0;
    int previous = art_level;
    if (target > art_level)
        art_level += (target - art_level + 1) / 2;
    else if (target < art_level)
        art_level -= (art_level - target + 3) / 4;
    bool changed = previous != art_level;
    if (art_level > 0) art_phase = (art_phase + 1) % 64;
    static const int peaks[9] = {20, 36, 56, 38, 68, 48, 30, 52, 22};
    for (int i = 0; i < 9; i++) {
        int phase = (art_phase * (1 + i % 2) + i * 7) % 32;
        int wave = phase < 16 ? phase : 32 - phase;
        int h = 8 + (peaks[i] - 8) * art_level * (80 + wave * 2) / (255 * 112);
        changed |= live_heights[i] != h;
        live_heights[i] = h;
    }
    if (changed) {
        // Only the fixed illustration rectangle. Headers, labels and controls
        // never participate in animation or get their layout recalculated.
        lv_area_t area;
        lv_obj_get_coords(body_surface, &area);
        area.x1 += 54;
        area.x2 = area.x1 + 116;
        area.y1 += 2;
        area.y2 = area.y1 + 114;
        lv_obj_invalidate_area(body_surface, &area);
    }
}

// Protocol and diagnostic states stay in English; only their compact display labels change.
static const char *state_text(const char *state)
{
    static const struct {
        const char *code, *text;
    } states[] = {
        {"STARTING", "STARTING"},
        {"READY", "READY"},
        {"CONNECTION LOST", "OFFLINE"},
        {"RECONNECTING", "RECONNECTING"},
        {"CONNECTING", "CONNECTING"},
        {"CONNECTING VOICE", "CONNECTING"},
        {"LISTENING", "LISTENING"},
        {"SPEAKING", "SPEAKING"},
        {"RECORDING", "RECORDING"},
        {"LIVE CHATTING", "LIVE"},
        {"LIVE LISTENING", "LISTENING"},
        {"RECORDING TOO LONG", "INPUT LIMIT"},
        {"HOLD TO TALK", "TAP TO SPEAK"},
        {"THINKING", "THINKING"},
        {"VOICE UNAVAILABLE", "UNAVAILABLE"},
        {"CHAT ENDED", "SESSION ENDED"},
        {"CHAT ACTIVE", "READY"},
        {"SERVER NOT CONNECTED", "OFFLINE"},
        {"NEW CONVERSATION", "NEW SESSION"},
        {"SETUP UNAVAILABLE", "SETUP ERROR"},
        {"STOPPED", "PAUSED"},
        {"WI-FI RECONNECTING", "RECONNECTING"},
        {"WI-FI SAVED", "WI-FI SAVED"},
        {"CONNECTING WI-FI", "CONNECTING"},
        {"SETTINGS UNAVAILABLE", "SETUP ERROR"},
        {"UPDATING", "UPDATING"},
        {"UPDATE FAILED", "UPDATE FAILED"},
        {"UPDATE COMPLETE", "UPDATED"},
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++)
        if (!strcmp(state, states[i].code))
            return states[i].text;
    return state;
}

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

static lv_obj_t *panel(int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_style_text_font(screen, &ui_font_18, 0);
    lv_obj_t *title = lv_image_create(screen);
    lv_image_set_src(title, &ui_wordmark);
    lv_obj_set_pos(title, 10, 8);
    panel(0, 45, 240, 3, UI_TEXT, 0);
    mode_label = label(12, 51, 216, 18, UI_ACCENT);
    lv_obj_set_style_text_font(mode_label, &ui_font_16, 0);
    lv_label_set_text(mode_label, "VOICE");
    battery_label = label(144, 6, 90, 20, UI_TEXT);
    volume_label = label(144, 26, 90, 20, UI_TEXT);
    lv_obj_set_style_text_font(battery_label, &ui_font_14, 0);
    lv_obj_set_style_text_font(volume_label, &ui_font_14, 0);
    status_surface = panel(8, 74, 224, 35, UI_SELECTION, 0);
    lv_obj_set_style_border_width(status_surface, 2, 0);
    lv_obj_set_style_border_color(status_surface, lv_color_hex(UI_TEXT), 0);
    status_label = label(17, 78, 210, 29, UI_TEXT);
    lv_obj_set_style_text_font(status_label, &ui_font_22, 0);
    body_surface = panel(8, 115, 224, 147, UI_CARD, 0);
    lv_obj_set_style_border_width(body_surface, 0, 0);
    lv_obj_add_event_cb(body_surface, draw_art, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_set_style_border_color(body_surface, lv_color_hex(UI_SURFACE), 0);
    selection = panel(10, 119, 220, 25, UI_ACCENT, 0);
    lv_obj_set_style_border_side(selection, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(selection, 4, 0);
    lv_obj_set_style_border_color(selection, lv_color_hex(UI_SELECTION), 0);
    lv_obj_set_hidden(selection, true);
    guide = label(18, 127, 208, 128, UI_BACKGROUND);
    detail_label = label(0, 270, 240, 50, UI_BACKGROUND);
    lv_obj_set_style_text_font(detail_label, &ui_font_14, 0);
    lv_obj_set_style_bg_color(detail_label, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_bg_opa(detail_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(detail_label, 12, 0);
    lv_obj_set_style_pad_top(detail_label, 4, 0);
    lv_obj_set_style_text_line_space(detail_label, 2, 0);
    volume_popup = label(44, 179, 152, 48, UI_BACKGROUND);
    lv_obj_set_style_text_align(volume_popup, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(volume_popup, &ui_font_22, 0);
    lv_obj_set_style_pad_top(volume_popup, 10, 0);
    lv_obj_set_style_bg_color(volume_popup, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(volume_popup, 3, 0);
    lv_obj_set_style_border_color(volume_popup, lv_color_hex(UI_SELECTION), 0);
    lv_obj_set_hidden(volume_popup, true);
    lv_timer_create(animate_art, 80, NULL);
    last_touch = esp_timer_get_time();
    bsp_lvgl_unlock();
    ui_show_guide();
    ui_battery(-1, settings_get().volume);
    ui_set("STARTING", "Starting audio / Wi-Fi");
}

void ui_set(const char *state, const char *detail)
{
    if (!status_label || !bsp_lvgl_lock(100))
        return;
    // Keep menus and QR pages stable while background network events arrive.
    if ((!qr || lv_obj_is_hidden(qr)) && !menu_visible) {
        const char *text = state_text(state);
        if (maintenance_visible) {
            lv_label_set_text(status_label, text);
            lv_label_set_text(detail_label, detail);
            bsp_lvgl_unlock();
            return;
        }
        if (strcmp(lv_label_get_text(status_label), text)) {
            lv_label_set_text(status_label, text);
            lv_obj_invalidate(body_surface);
        }
        // The guide already keeps the back gesture visible; avoid a duplicate footer.
        const char *shown_detail =
            (!*detail || !strcmp(detail, "Hold OK: home") ||
             !strcmp(detail, "OK: start recording") || !strcmp(detail, "OK: send\nHold OK: home"))
                ? "UP/DN: volume / Hold OK: home"
                : detail;
        if (!strcmp(text, "RECORDING"))
            lv_label_set_text(guide, "Recording / OK sends your message");
        else if (!strcmp(text, "SPEAKING"))
            lv_label_set_text(guide, "Replying / OK stops reply");
        else if (!strcmp(text, "OFFLINE") || !strcmp(text, "UNAVAILABLE") ||
                 !strcmp(text, "SETUP ERROR"))
            lv_label_set_text(guide, "Check connection and try again");
        else if (strstr(text, "CONNECTING"))
            lv_label_set_text(guide, "Getting your voice session ready");
        else if (!strcmp(text, "UPDATING"))
            lv_label_set_text(guide, "Keep your device powered on");
        else
            lv_label_set_text(guide, live_mode ? "Speak freely / OK stops reply"
                                               : "OK records / tap again to send");
        lv_obj_set_y(detail_label, strchr(shown_detail, '\n') ? 284 : 288);
        lv_obj_set_height(detail_label, strchr(shown_detail, '\n') ? 36 : 32);
        if (strcmp(lv_label_get_text(detail_label), shown_detail))
            lv_label_set_text(detail_label, shown_detail);
    }
    bsp_lvgl_unlock();
}

void ui_battery(int percent, int volume)
{
    if (!bsp_lvgl_lock(100))
        return;
    if (percent != shown_battery) {
        if (percent >= 0 && percent <= 100)
            lv_label_set_text_fmt(battery_label, "BAT %d%%", percent);
        else
            lv_label_set_text(battery_label, "BAT --");
        lv_obj_set_style_text_color(
            battery_label, lv_color_hex(percent >= 0 && percent <= 15 ? UI_WARNING : UI_TEXT), 0);
        shown_battery = percent;
    }
    if (volume != shown_volume) {
        lv_label_set_text_fmt(volume_label, "VOL %d", volume);
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
    if (active || setup_visible || (menu_visible && !home_visible)) {
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

static void close_home(void)
{
    home_visible = false;
    lv_obj_set_style_bg_color(status_surface, lv_color_hex(UI_SELECTION), 0);
    lv_obj_set_style_border_width(status_surface, 0, 0);
    lv_obj_set_style_text_color(guide, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_style_text_font(guide, &ui_font_18, 0);
    lv_obj_set_hidden(mode_label, false);
    lv_obj_set_hidden(status_label, false);
    lv_obj_set_style_text_font(status_label, &ui_font_22, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(mode_label, &ui_font_16, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_pos(mode_label, 12, 51);
    lv_obj_set_size(mode_label, 216, 18);
    if (home_surface) {
        lv_obj_delete(home_surface);
        home_surface = NULL;
    }
    lv_obj_set_pos(detail_label, 0, 270);
    lv_obj_set_size(detail_label, 240, 50);
    lv_obj_set_style_text_font(detail_label, &ui_font_14, 0);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_LEFT, 0);
}

// One draw surface instead of twelve styled objects leaves room for QR allocation
// in the existing 24 KiB pool. Text is static and no bitmap buffer is allocated.
static void draw_home_text(lv_layer_t *layer, int x, int y, int width, int height,
                           const lv_font_t *font, const char *text, uint32_t color)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.text = text;
    dsc.color = lv_color_hex(color);
    lv_area_t area = {x, y, x + width - 1, y + height - 1};
    lv_draw_label(layer, &dsc, &area);
}

// Flat native drawing avoids bitmaps and temporary layers in the 24 KiB pool.
static void ink_rect(lv_layer_t *layer, int x, int y, int w, int h, uint32_t color, int radius)
{
    if (w <= 0 || h <= 0)
        return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = radius;
    dsc.bg_color = lv_color_hex(color);
    lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &dsc, &area);
}

static void comic_corner(lv_layer_t *layer, int x, int y, int h, uint32_t color)
{
    // A small diagonal print edge, with sparse halftone dots at native resolution.
    for (int k = 0; k < h; k++) {
        int w = 6 + k * 18 / h;
        ink_rect(layer, x + 24 - w, y + k, w, 1, color, 0);
    }
    for (int k = 8; k < h - 3; k += 9) {
        ink_rect(layer, x + 19, y + k, 3, 3, UI_BACKGROUND, 2);
        if (k > h / 2)
            ink_rect(layer, x + 12, y + k, 2, 2, UI_BACKGROUND, 1);
    }
}

static void ink_arc(lv_layer_t *layer, int x, int y, int radius, int start, int end, int width,
                    uint32_t color)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center.x = x;
    dsc.center.y = y;
    dsc.radius = radius;
    dsc.start_angle = start;
    dsc.end_angle = end;
    dsc.width = width;
    dsc.color = lv_color_hex(color);
    dsc.rounded = true;
    lv_draw_arc(layer, &dsc);
}

static void draw_art(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t a;
    lv_obj_get_coords(body_surface, &a);
    int x = a.x1, y = a.y1;
    if (art_kind == 2) {
        uint32_t accent = menu_voice_picker ? UI_MINT : UI_SELECTION;
        for (int i = 0; i < menu_rows_count; i++) {
            int row_y = y + i * menu_row_height;
            bool focused = i == menu_cursor;
            const char *text = menu_rows[i];
            bool active = menu_voice_picker && !strncmp(text, "* ", 2);
            if (active)
                text += 2;
            if (focused) {
                ink_rect(layer, x + 1, row_y, 215, menu_row_height - 2, 0x29292c, 4);
                ink_rect(layer, x + 1, row_y + 5, 3, menu_row_height - 12, accent, 1);
            }
            const lv_font_t *font = focused ? &ui_font_22 : &ui_font_18;
            draw_home_text(layer, x + 13, row_y + (focused ? 2 : 6), 181, font->line_height, font,
                           text, focused ? UI_TEXT : UI_MUTED);
            if (active) {
                ink_rect(layer, x + 181, row_y + 7, 27, 15, UI_MINT, 7);
                draw_home_text(layer, x + 188, row_y + 7, 24, 16, &ui_font_14, "ON", UI_BACKGROUND);
            } else if (focused) {
                draw_home_text(layer, x + 200, row_y + 6, 14, 18, &ui_font_14, ">", accent);
            }
            if (!focused && i < menu_rows_count - 1)
                ink_rect(layer, x + 13, row_y + menu_row_height - 2, 194, 1, 0x303033, 0);
        }
        if (menu_total > 6) {
            ink_rect(layer, x + 222, y + 3, 2, 156, 0x303033, 1);
            ink_rect(layer, x + 222, y + 3 + menu_first * 156 / menu_total, 2, 6 * 156 / menu_total,
                     accent, 1);
        }
        return;
    }
    if (art_kind == 3) {
        ink_arc(layer, x + 112, y + 66, 48, 135, 45, 7, UI_SURFACE);
        if (art_value > 0)
            ink_arc(layer, x + 112, y + 66, 48, 135, (135 + art_value * 270 / 100) % 360, 7,
                    UI_ACCENT);
        draw_home_text(layer, x + 87, y + 53, 70, 30, &ui_font_22, art_readout, UI_TEXT);
        return;
    }
    if (art_kind == 4) {
        ink_rect(layer, x + 15, y + 15, 52, 52, UI_ORANGE, 26);
        draw_home_text(layer, x + 34, y + 22, 20, 30, &ui_font_22, "!", UI_BACKGROUND);
        draw_home_text(layer, x + 80, y + 30, 140, 20, &ui_font_14, "PLEASE CONFIRM", UI_ORANGE);
        return;
    }
    const char *state = lv_label_get_text(status_label);
    bool talking = !strcmp(state, "SPEAKING");
    bool waiting = !strcmp(state, "THINKING") || strstr(state, "CONNECTING");
    uint32_t color = waiting         ? UI_ORANGE
                     : talking       ? UI_ACCENT
                     : art_kind == 1 ? UI_MINT
                                     : UI_SELECTION;
    if (art_kind == 0) {
        ink_arc(layer, x + 112, y + 56, 49, 0, 359, 2, UI_LILAC);
        int span = 28 + art_level * 140 / 255;
        ink_arc(layer, x + 112, y + 56, 42, 270 - span / 2, 270 + span / 2, 5, color);
        ink_rect(layer, x + 100, y + 31, 24, 39, color, 12);
        ink_arc(layer, x + 112, y + 55, 20, 0, 180, 3, UI_TEXT);
        ink_rect(layer, x + 110, y + 76, 4, 10, UI_TEXT, 2);
        ink_rect(layer, x + 102, y + 86, 20, 3, UI_TEXT, 1);
        for (int i = 0; i < 4; i++) {
            ink_rect(layer, x + 20 + i * 6, y + 30 + (i % 2) * 8, 2, 2, UI_ACCENT, 1);
            ink_rect(layer, x + 181 + i * 6, y + 76 + (i % 2) * 8, 2, 2, UI_LILAC, 1);
        }
    } else {
        ink_arc(layer, x + 112, y + 58, 54, 195, 340, 2, UI_LILAC);
        ink_arc(layer, x + 112, y + 58, 54, 15, 160, 2, UI_LILAC);
        for (int i = 0; i < 9; i++) {
            int h = live_heights[i];
            ink_rect(layer, x + 57 + i * 13, y + 58 - h / 2, 7, h, color, 4);
        }
    }
    if (waiting)
        for (int i = 0; i < 3; i++)
            ink_rect(layer, x + 99 + i * 12, y + 117, 5, 5, UI_ORANGE, 3);
}

static void draw_home(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t bounds;
    lv_obj_get_coords(home_surface, &bounds);
    int y = bounds.y1;
    for (int i = 0; i < 3; i++) {
        bool focused = i == home_selected;
        int h = 60 + 38 * home_weights[i] / 1024;
        int x = bounds.x1 + 4;
        uint32_t accent = i == 0 ? UI_SELECTION : i == 1 ? UI_MINT : UI_LILAC;
        if (i > 0)
            ink_rect(layer, x, y, 232, 2, UI_TEXT, 0);
        if (focused)
            ink_rect(layer, x, y + 8, 3, h - 18, UI_ACCENT, 0);
        comic_corner(layer, x + 201, y + 6, h - 12, accent);
        const lv_font_t *font = focused && h >= 92
                                    ? (i == 2 ? &ui_font_comic_52 : &ui_font_comic_80)
                                    : &ui_font_comic_44;
        draw_home_text(layer, x + 10, y + (focused ? 7 : 4), 207, font->line_height, font,
                       home_titles[i], focused ? accent : UI_TEXT);
        draw_home_text(layer, x + 12, y + h - 24, 196, 20, &ui_font_18, home_descriptions[i],
                       UI_MUTED);
        y += h + 4;
    }
}

void ui_show_home(int selected)
{
    ui_wake();
    if (!bsp_lvgl_lock(100))
        return;
    maintenance_visible = false;
    if (qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    setup_visible = false;
    menu_visible = home_visible = true;
    lv_obj_set_hidden(volume_popup, true);
    lv_obj_set_hidden(selection, true);
    lv_obj_set_hidden(status_surface, true);
    lv_obj_set_hidden(guide, true);
    lv_obj_set_hidden(body_surface, true);
    lv_obj_set_hidden(mode_label, true);
    lv_obj_set_hidden(status_label, true);
    lv_label_set_text(status_label, "SELECT MODE");
    int next = selected >= 0 && selected < 3 ? selected : 0;
    // Commit selection and its final geometry together. Interpolating row height while
    // switching discrete bitmap font sizes caused a second visible jump near the end.
    home_selected = next;
    for (int i = 0; i < 3; i++)
        home_weights[i] = i == next ? 1024 : 0;
    if (!home_surface) {
        home_surface = panel(0, 53, 240, 229, UI_BACKGROUND, 0);
        lv_obj_add_event_cb(home_surface, draw_home, LV_EVENT_DRAW_MAIN, NULL);
    }
    lv_obj_invalidate(home_surface);
    lv_obj_set_pos(detail_label, 0, 293);
    lv_obj_set_size(detail_label, 240, 27);
    lv_obj_set_style_text_font(detail_label, &ui_font_14, 0);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(detail_label, "UP/DN  /  OK ENTER");
    bsp_lvgl_unlock();
}

static void show_qr(const char *title, const char *payload, const char *instructions)
{
    ui_wake();
    if (!bsp_lvgl_lock(100))
        return;
    close_home();
    maintenance_visible = false;
    lv_obj_set_hidden(volume_popup, true);
    lv_obj_set_hidden(selection, true);
    lv_obj_set_hidden(status_surface, false);
    lv_obj_set_hidden(body_surface, true);
    lv_obj_set_pos(status_label, 17, 78);
    lv_obj_set_size(status_label, 208, 30);
    setup_visible = true;
    lv_obj_set_hidden(guide, true);
    lv_label_set_text(mode_label, "GET CONNECTED");
    lv_obj_set_style_bg_color(status_surface, lv_color_hex(UI_MINT), 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_BACKGROUND), 0);
    lv_label_set_text(status_label, title);
    lv_label_set_text(detail_label, instructions);
    if (!qr) {
        qr = lv_qrcode_create(lv_screen_active());
        lv_qrcode_set_size(qr, 144);
        lv_qrcode_set_quiet_zone(qr, true);
        lv_obj_set_pos(qr, 48, 119);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
    }
    // Reuse between QR steps; release when leaving QR so large titles have room.
    lv_obj_set_hidden(qr, false);
    lv_obj_move_foreground(qr);
    if (lv_qrcode_update(qr, payload, strlen(payload)) != LV_RESULT_OK) {
        lv_label_set_text(status_label, "QR ERROR");
        lv_label_set_text(detail_label, "Go back and retry");
    }
    bsp_lvgl_unlock();
}

void ui_show_setup(const char *ssid, bool website)
{
    char payload[96], detail[128];
    if (website) {
        show_qr("02 / SET UP WI-FI", "http://192.168.4.1/", "Open Wi-Fi sign-in\nOr scan this backup");
    } else {
        snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
        snprintf(detail, sizeof(detail), "%s\nScan to join and set up", ssid);
        show_qr("01 / JOIN HOTSPOT", payload, detail);
    }
}

void ui_show_guide(void)
{
    if (!bsp_lvgl_lock(100))
        return;
    art_level = art_phase = 0;
    for (int i = 0; i < 9; i++) live_heights[i] = 8;
    atomic_store(&audio_meter[0], 0);
    atomic_store(&audio_meter[1], 0);
    close_home();
    if (qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    setup_visible = false;
    menu_visible = false;
    maintenance_visible = false;
    lv_obj_set_hidden(selection, true);
    lv_obj_set_hidden(status_surface, false);
    lv_obj_set_pos(status_label, 17, 78);
    lv_obj_set_size(status_label, 210, 29);
    lv_label_set_text(mode_label, live_mode ? "LIVE" : "VOICE");
    art_kind = live_mode ? 1 : 0;
    lv_obj_set_style_text_font(mode_label, &ui_font_comic_44, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(live_mode ? UI_MINT : UI_SELECTION), 0);
    lv_obj_set_pos(mode_label, 12, 51);
    lv_obj_set_size(mode_label, 216, 44);
    lv_obj_set_hidden(status_surface, true);
    lv_obj_set_pos(status_label, 14, 99);
    lv_obj_set_size(status_label, 212, 27);
    lv_obj_set_style_bg_color(status_surface, lv_color_hex(live_mode ? UI_MINT : UI_SELECTION), 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_hidden(body_surface, false);
    lv_obj_set_pos(body_surface, 8, 128);
    lv_obj_set_size(body_surface, 224, 132);
    lv_obj_set_style_bg_color(body_surface, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_pos(guide, 12, 263);
    lv_obj_set_size(guide, 216, 20);
    lv_obj_set_style_text_font(guide, &ui_font_14, 0);
    lv_obj_set_style_text_color(guide, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_line_space(guide, 0, 0);
    lv_label_set_text(guide, live_mode ? "Speak freely / OK stops reply"
                                       : "OK records / tap again to send");
    lv_label_set_text(detail_label, "UP/DN: volume / Hold OK: home");
    lv_obj_set_hidden(guide, false);
    bsp_lvgl_unlock();
}

static void show_maintenance(const char *title)
{
    ui_wake();
    if (!bsp_lvgl_lock(100))
        return;
    close_home();
    if (qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    setup_visible = menu_visible = false;
    maintenance_visible = true;
    // Hide chat artwork and controls throughout checking, download and result.
    lv_obj_set_hidden(volume_popup, true);
    lv_obj_set_hidden(selection, true);
    lv_obj_set_hidden(status_surface, true);
    lv_obj_set_hidden(body_surface, true);
    lv_obj_set_hidden(guide, true);
    lv_label_set_text(mode_label, title);
    lv_obj_set_style_text_font(mode_label, &ui_font_22, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(UI_MINT), 0);
    lv_obj_set_pos(mode_label, 12, 56);
    lv_obj_set_size(mode_label, 216, 32);
    lv_obj_set_pos(status_label, 12, 118);
    lv_obj_set_size(status_label, 216, 76);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_pos(detail_label, 0, 218);
    lv_obj_set_size(detail_label, 240, 90);
    lv_label_set_text(status_label, "");
    lv_label_set_text(detail_label, "");
    bsp_lvgl_unlock();
}

void ui_show_update(void)
{
    show_maintenance("FIRMWARE");
    ui_set("CHECKING UPDATE", "Please wait\nKeep power connected");
}

void ui_show_network(void)
{
    show_maintenance("WI-FI");
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
    close_home();
    if (qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    lv_obj_set_hidden(volume_popup, true);
    menu_visible = true;
    setup_visible = false;
    maintenance_visible = false;
    lv_obj_set_hidden(status_surface, true);
    lv_obj_set_hidden(body_surface, true);
    lv_obj_set_pos(status_label, 12, 78);
    lv_obj_set_size(status_label, 208, 30);
    lv_label_set_text(mode_label, "SETTINGS");
    lv_obj_set_style_text_font(mode_label, &ui_font_14, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(UI_MUTED), 0);
    art_kind = 2;
    lv_obj_set_style_bg_color(status_surface, lv_color_hex(UI_LILAC), 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_bg_color(body_surface, lv_color_hex(UI_BACKGROUND), 0);
    lv_obj_set_hidden(guide, false);
    lv_obj_set_hidden(body_surface, false);
    lv_obj_set_pos(body_surface, 8, 115);
    lv_obj_set_size(body_surface, 224, 152);
    lv_obj_set_pos(guide, 18, 120);
    lv_obj_set_size(guide, 208, 150);
    lv_obj_set_style_text_line_space(guide, 25 - ui_font_18.line_height, 0);
    // Six fixed-height rows, no animated scrolling or per-frame allocation.
    char text[512] = {0};
    size_t used = 0;
    int first = selected >= 6 ? selected - 5 : 0;
    for (int i = first; i < count && i < first + 6; i++) {
        size_t n = strlen(items[i]);
        // Append complete UTF-8 strings; never truncate halfway through a glyph.
        if (used + n + 2 > sizeof(text))
            break;
        if (used)
            text[used++] = '\n';
        memcpy(text + used, items[i], n + 1);
        used += n;
    }
    lv_anim_delete(selection, NULL);
    lv_obj_set_y(selection, 119 + (selected - first) * 25);
    lv_obj_set_hidden(selection, false);
    if (!strcmp(title, "ADJUST")) {
        art_kind = 3;
        const char *number = strrchr(items[0], ':');
        int value = number ? atoi(number + 1) : 0;
        int max = strstr(items[0], "Screen") ? 600 : strstr(items[0], "Idle") ? 900 : 100;
        snprintf(art_readout, sizeof(art_readout), "%d%s", value, max == 100 ? "%" : "s");
        art_value = value * 100 / max;
        if (art_value < 0)
            art_value = 0;
        if (art_value > 100)
            art_value = 100;
        lv_obj_set_style_bg_color(body_surface, lv_color_hex(UI_BACKGROUND), 0);
        lv_obj_set_hidden(selection, true);
        lv_obj_set_pos(guide, 18, 234);
        lv_obj_set_size(guide, 208, 25);
        lv_obj_set_style_text_color(guide, lv_color_hex(UI_TEXT), 0);
    } else if (strchr(title, '?')) {
        art_kind = 4;
        lv_obj_set_style_bg_color(body_surface, lv_color_hex(UI_BACKGROUND), 0);
        lv_obj_set_style_text_color(status_label, lv_color_hex(UI_ORANGE), 0);
        lv_obj_set_pos(guide, 18, 203);
        lv_obj_set_size(guide, 208, 60);
        lv_obj_set_style_text_color(guide, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_y(selection, 202 + (selected - first) * 25);
        lv_obj_set_style_bg_color(selection, lv_color_hex(UI_SURFACE), 0);
    } else {
        menu_root = !strcmp(title, "SETTINGS");
        menu_voice_picker = !strcmp(title, "VOICE SELECT");
        menu_rows_count = 0;
        menu_cursor = selected - first;
        menu_first = first;
        menu_total = count;
        menu_row_height = menu_root ? 32 : 27;
        for (int i = first; i < count && i < first + 6; i++)
            snprintf(menu_rows[menu_rows_count++], sizeof(menu_rows[0]), "%s", items[i]);
        lv_obj_set_hidden(guide, true);
        lv_obj_set_hidden(selection, true);
        lv_obj_set_pos(body_surface, 8, 108);
        lv_obj_set_size(body_surface, 224, 162);
        if (menu_root) {
            lv_obj_set_hidden(mode_label, true);
            lv_obj_set_style_text_font(status_label, &ui_font_comic_44, 0);
            lv_obj_set_pos(status_label, 12, 65);
            lv_obj_set_size(status_label, 216, 36);
        } else {
            lv_obj_set_style_text_color(status_label,
                                        lv_color_hex(menu_voice_picker ? UI_MINT : UI_TEXT), 0);
        }
    }
    lv_obj_invalidate(body_surface);
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
    lv_label_set_text_fmt(volume_popup, "VOL  %d", volume);
    lv_obj_set_hidden(volume_popup, false);
    lv_obj_move_foreground(volume_popup);
    bsp_lvgl_unlock();
}
