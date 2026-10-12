#include "lvgl.h"
#include "settings.h"
#include "menu.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include PREVIEW_UI_SOURCE
static uint16_t pixels[240 * 320];
static uint16_t draw[240 * 20] __attribute__((aligned(64)));
static settings_t prefs = {
    .volume = 100, .brightness = 65, .screen_seconds = 60, .idle_seconds = 180, .duplex = 1};
static int errors, captures;
static void log_message(lv_log_level_t level, const char *message)
{
    fputs(message, stderr);
    if (level == LV_LOG_LEVEL_ERROR || strstr(message, "No memory") ||
        strstr(message, "Failed to allocate") || strstr(message, "No draw buffer"))
        errors++;
}
static int64_t clock_us = 1000000;
static uint8_t backlight;
settings_t settings_get(void)
{
    return prefs;
}
esp_err_t settings_save(settings_t p)
{
    prefs = p;
    return 0;
}
const char *settings_voice(int i)
{
    static const char *v[] = {"cove", "juniper", "maple", "spruce", "ember",
                              "vale", "breeze",  "arbor", "sol"};
    return v[i];
}
bool voice_wifi_connected(void)
{
    return true;
}
void voice_wifi_ip(char *s, size_t n)
{
    snprintf(s, n, "192.168.7.17");
}
esp_err_t bsp_display_init(void)
{
    return 0;
}
int64_t esp_timer_get_time(void)
{
    return clock_us;
}
void bsp_display_backlight(uint8_t percent)
{
    backlight = percent;
}
bool bsp_lvgl_lock(int timeout_ms)
{
    (void)timeout_ms;
    return true;
}
void bsp_lvgl_unlock(void) {}
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    uint16_t *src = (uint16_t *)p;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            pixels[y * 240 + x] = *src++;
    lv_display_flush_ready(d);
}
lv_display_t *bsp_lvgl_init(void)
{
    lv_init();
    lv_log_register_print_cb(log_message);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, draw, NULL, sizeof(draw), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    return d;
}
static void check_label(lv_obj_t *obj, const char *page)
{
    if (lv_obj_is_hidden(obj))
        return;
    lv_point_t size;
    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    lv_text_get_size(&size, lv_label_get_text(obj), font, 0,
                     lv_obj_get_style_text_line_space(obj, 0), 10000, 0);
    if (size.x > lv_obj_get_content_width(obj) || size.y > lv_obj_get_content_height(obj)) {
        fprintf(stderr, "CLIP %s: %s needs %dx%d has %dx%d\n", page, lv_label_get_text(obj),
                (int)size.x, (int)size.y, (int)lv_obj_get_content_width(obj),
                (int)lv_obj_get_content_height(obj));
        errors++;
    }
}
static void check_tree(lv_obj_t *obj, const char *page)
{
    if (lv_obj_is_hidden(obj))
        return;
    if (lv_obj_check_type(obj, &lv_label_class))
        check_label(obj, page);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        check_tree(lv_obj_get_child(obj, i), page);
}
static void capture_frame(const char *name, bool settle)
{
    captures++;
    if (settle) { lv_tick_inc(200); lv_timer_handler(); }
    lv_refr_now(NULL);
    check_tree(lv_screen_active(), name);
    if (home_visible) {
        for (int i = 0; i < 3; i++) {
            lv_point_t extent;
            int h = 60 + 38 * home_weights[i] / 1024;
            const lv_font_t *font = i == home_selected && h >= 92
                                        ? (i == 2 ? &ui_font_comic_52 : &ui_font_comic_80) : &ui_font_comic_44;
            lv_text_get_size(&extent, home_titles[i], font, 0, 0, 10000, 0);
            if (extent.x > 207 || extent.y > h - 30) fprintf(stderr, "HOME CLIP %s %s %dx%d h=%d\n", name, home_titles[i], (int)extent.x, (int)extent.y, h);
            assert(extent.x <= 207 && extent.y <= h - 30);
            lv_text_get_size(&extent, home_descriptions[i], &ui_font_18, 0, 0, 10000, 0);
            assert(extent.x <= 196 && extent.y <= 20);
        }
    }
    if (menu_visible && !home_visible && art_kind == 2 && !lv_obj_is_hidden(body_surface)) {
        for (int i = 0; i < menu_rows_count; i++) {
            lv_point_t extent;
            const char *text = menu_rows[i];
            bool active = menu_voice_picker && !strncmp(text, "* ", 2);
            if (active) text += 2;
            const lv_font_t *font = i == menu_cursor ? &ui_font_22 : &ui_font_18;
            lv_text_get_size(&extent, text, font, 0, 0, 10000, 0);
            assert(extent.x <= (active ? 162 : 181));
            assert(extent.y <= menu_row_height - 2);
        }
    }
    if (qr)
        assert(lv_qrcode_is_render_valid(qr));
    char path[100];
    snprintf(path, sizeof(path), "%s.ppm", name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n240 320\n255\n");
    for (unsigned i = 0; i < 240 * 320; i++) {
        unsigned p = pixels[i];
        unsigned char rgb[] = {((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63,
                               (p & 31) * 255 / 31};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static void capture(const char *name) { capture_frame(name, true); }
static void advance_frame(void)
{
    lv_tick_inc(20);
    clock_us += 20000;
    lv_timer_handler();
}
static void settings_root(void)
{
    menu_open(2);
    menu_key(0);
}
static void navigation_cycle(int selected)
{
    ui_show_guide();
    menu_open(selected);
    ui_show_setup("mic-on-FFFF", true);
    lv_refr_now(NULL);
    assert(lv_qrcode_is_render_valid(qr));
    ui_show_home(selected);
    lv_refr_now(NULL);
}
static void test_audio_animation(void)
{
    int16_t pcm[480];
    uint16_t baseline[240 * 320];
    // DC and small background noise must not animate.
    for (int i = 0; i < 480; i++) pcm[i] = 1200;
    ui_audio_samples(false, pcm, 480);
    assert(meter_level(false) == 0);
    for (int i = 0; i < 480; i++) pcm[i] = (i / 4 % 2) ? 80 : -80;
    ui_audio_samples(false, pcm, 480);
    assert(meter_level(false) == 0);
    for (int mode = 0; mode < 2; mode++) {
        ui_mode(mode);
        ui_show_guide();
        ui_set(mode ? "LIVE LISTENING" : "RECORDING", "Hold OK: home");
        lv_refr_now(NULL);
        memcpy(baseline, pixels, sizeof(pixels));
        for (int f = 0; f < 48; f++) {
            int amplitude = f < 32 ? 400 + (f % 16) * 650 : 0;
            for (int i = 0; i < 480; i++) pcm[i] = (i / 4 % 2) ? amplitude : -amplitude;
            ui_audio_samples(false, pcm, 480);
            for (int tick = 0; tick < 4; tick++) advance_frame();
            char name[48];
            snprintf(name, sizeof(name), "audio-%d-%02d", mode, f);
            capture_frame(name, false);
            // Exact pixel stability outside the illustration, including all text.
            for (int y = 0; y < 320; y++)
                for (int x = 0; x < 240; x++)
                    if (x < 62 || x > 178 || y < 130 || y > 244)
                        assert(pixels[y * 240 + x] == baseline[y * 240 + x]);
            for (int i = 0; i < 9; i++) assert(live_heights[i] >= 8 && live_heights[i] <= 68);
        }
        for (int i = 0; i < 40; i++) advance_frame();
        assert(art_level == 0);
        for (int i = 0; i < 9; i++) assert(live_heights[i] == 8);
    }
    // Separate input/output sources; stale frames settle without further PCM.
    for (int i = 0; i < 480; i++) pcm[i] = (i / 4 % 2) ? 12000 : -12000;
    ui_set("SPEAKING", "Hold OK: home");
    ui_audio_samples(false, pcm, 480);
    animate_art(NULL);
    assert(art_level == 0);
    ui_audio_samples(true, pcm, 480);
    animate_art(NULL);
    assert(art_level > 0);
    for (int i = 0; i < 120; i++) advance_frame();
    assert(art_level == 0);
    ui_audio_samples(true, pcm, 480);
    ui_show_home(0);
    capture("audio-menu-before");
    memcpy(baseline, pixels, sizeof(pixels));
    for (int i = 0; i < 20; i++) advance_frame();
    lv_refr_now(NULL);
    assert(art_level == 0 && !memcmp(baseline, pixels, sizeof(pixels)));
    ui_show_guide();
    ui_set("RECORDING", "Hold OK: home");
    ui_audio_samples(false, pcm, 480);
    screen_on = false;
    animate_art(NULL);
    assert(art_level == 0);
    ui_wake();
    printf("Audio animation: DC/noise gate, PCM sources, silence/stale decay, fixed text, menu and sleep checks passed\n");
}

int main(int argc, char **argv)
{
    ui_init();
    ui_battery(86, 70);
    if (argc == 2 && !strcmp(argv[1], "--update-only")) {
        ui_show_update();
        capture("update-checking");
        ui_set("DOWNLOADING UPDATE", "New version / 45%\nKeep power connected");
        capture("update-downloading");
        ui_set("UP TO DATE", "Latest version installed\nOK: back");
        capture("update-current");
        ui_set("UPDATE NOT INSTALLED", "Download interrupted\nOK: back");
        capture("update-failed");
        return errors ? 1 : 0;
    }
    test_audio_animation();
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "home-%d", i);
        menu_open(i);
        capture(name);
    }
    // Selection geometry must be final immediately and stay unchanged after timer ticks.
    ui_show_home(0);
    capture("home-switch-start");
    for (int target = 1; target <= 2; target++) {
        ui_show_home(target);
        assert(home_selected == target && home_weights[target] == 1024);
        for (int frame = 0; frame < 10; frame++) {
            char name[40];
            snprintf(name, sizeof(name), "switch-%d-%02d", target, frame);
            capture_frame(name, false);
            advance_frame();
            assert(home_selected == target && home_weights[target] == 1024);
        }
        assert(home_weights[target] == 1024);
    }
    for (int i = 0; i < 120; i++) {
        ui_show_home(i % 3);
        advance_frame();
        if (i % 4 == 0) ui_show_guide(); // Exercise rapid page replacement.
    }
    ui_show_home(2);
    capture("home-after-rapid-input");
    // Background connection events cannot overwrite navigation; home can still sleep.
    ui_set("RECONNECTING", "Connecting...");
    assert(!strcmp(lv_label_get_text(status_label), "SELECT MODE"));
    clock_us += 61000000;
    ui_tick(false);
    assert(backlight == 0);
    ui_wake();
    assert(backlight == 65);
    ui_mode(false);
    ui_show_guide();
    ui_set("HOLD TO TALK", "OK: start recording");
    capture("voice-ready");
    ui_set("RECORDING", "OK: send\nHold OK: home");
    capture("voice-recording");
    ui_set("SPEAKING", "Hold OK: home");
    capture("voice-speaking");
    static const char *states[] = {"STARTING", "CONNECTING", "THINKING", "RECONNECTING",
        "VOICE UNAVAILABLE", "CHAT ENDED", "RECORDING TOO LONG", "UPDATING",
        "UPDATE FAILED", "UPDATE COMPLETE", "SETUP UNAVAILABLE", "STOPPED", "APPLYING VOICE"};
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        char name[32]; snprintf(name, sizeof(name), "state-%02u", i);
        ui_set(states[i], "Hold OK: home"); capture(name);
    }
    ui_mode(true);
    ui_show_guide();
    ui_set("LIVE LISTENING", "Hold OK: home");
    capture("free-dialogue");
    settings_root();
    capture("settings");
    menu_key(0);
    capture("voices");
    menu_key(1);
    assert(menu_key(0) == MENU_VOICE_CHANGED);
    assert(prefs.voice == 1 && menu_visible && menu_cursor == 1);
    capture("voice-saved-stays");
    ui_set("RECONNECTING", "Connecting...");
    capture("voice-saved-network-event");
    assert(!strcmp(lv_label_get_text(status_label), "VOICE SELECT"));
    assert(menu_key(0) == MENU_NONE && menu_cursor == 1);
    capture("voice-saved-repeat-ok");
    menu_key(-1);
    assert(menu_key(0) == MENU_VOICE_CHANGED && prefs.voice == 0);
    for (int i = 0; i < 8; i++)
        menu_key(1);
    capture("voices-last");
    settings_root();
    menu_key(1);
    menu_key(0);
    capture("device");
    menu_key(0);
    capture("volume");
    for (int i = 0; i < 10; i++) menu_key(1);
    capture("volume-half");
    for (int i = 0; i < 10; i++) menu_key(1);
    capture("volume-zero");
    menu_back();
    const char *edits[] = {"brightness", "screen-timeout", "idle-timeout"};
    for (int i = 0; i < 3; i++) {
        menu_key(1); menu_key(0); capture(edits[i]); menu_back();
    }
    settings_root(); menu_key(1); menu_key(0);

    for (int i = 0; i < 4; i++)
        menu_key(1);
    menu_key(0);
    capture("update-confirm");
    settings_root();
    menu_key(1);
    menu_key(1);
    menu_key(0);
    capture("network");
    menu_key(0);
    capture("network-confirm");
    menu_back(); menu_key(1); menu_key(0); capture("connection-info");
    settings_root();
    for (int i = 0; i < 3; i++)
        menu_key(1);
    menu_key(0);
    capture("reset-context");
    ui_show_network();
    ui_set("WI-FI CONNECTED", "Finish on your phone");
    capture("wifi-connected");
    ui_set("WI-FI NOT CONNECTED", "Check phone and retry");
    capture("wifi-failed");
    ui_show_setup("mic-on-FFFF", false);
    capture("qr-hotspot");
    ui_show_setup("mic-on-FFFF", true);
    capture("qr-setup");
    ui_set("SPEAKING", "Hold OK: home");
    capture("qr-network-event");
    assert(!strcmp(lv_label_get_text(status_label), "02 / SET UP WI-FI"));
    ui_mode(false);
    ui_show_guide();
    ui_battery(9, 0);
    ui_set("CONNECTION LOST", "OK: retry\nHold OK: home");
    capture("low-battery");
    ui_battery(-1, 80);
    ui_set("READY", "OK: start recording");
    ui_volume(80);
    capture("volume-popup");
    lv_mem_monitor_t before, after;
    // Warm every path, including guide layout and QR decoding. Start and end on home 0.
    for (int i = 0; i < 4; i++)
        navigation_cycle(i % 3);
    lv_mem_monitor(&before);
    for (int i = 0; i < 100; i++)
        navigation_cycle(i % 3);
    lv_mem_monitor(&after);
    fprintf(stderr, "pool before=%zu after=%zu largest=%zu\n", before.free_size, after.free_size,
            after.free_biggest_size);
    assert(after.free_size >= before.free_size);
    printf("Rendered %d states, %d clipped labels; LVGL free %zu bytes, largest block %zu; 100 "
           "navigation cycles without leaks\n",
           captures, errors, after.free_size, after.free_biggest_size);
    return errors ? 1 : 0;
}
