#include "ui.h"
#include "settings.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "esp_check.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Flash-resident, 2 bpp subsets of the static UI text. No transcript or animation.
LV_FONT_DECLARE(ui_font_18);
LV_FONT_DECLARE(ui_font_22);
static lv_obj_t *status_label, *detail_label, *battery_label, *volume_label, *guide, *qr,
    *volume_popup, *mode_label, *selection;
static int64_t last_touch, volume_until;
static bool screen_on = true, setup_visible, menu_visible, live_mode;
static int shown_battery = -2, shown_volume = -1;

// Protocol and diagnostic states stay in English; only their presentation changes.
static const char *state_text(const char *state)
{
    static const struct {
        const char *code, *text;
    } states[] = {
        {"STARTING", "正在启动"},
        {"READY", "准备好了"},
        {"CONNECTION LOST", "连接已断开"},
        {"RECONNECTING", "正在重新连接"},
        {"CONNECTING", "正在连接服务"},
        {"CONNECTING VOICE", "正在连接语音"},
        {"LISTENING", "正在聆听"},
        {"SPEAKING", "正在回答"},
        {"RECORDING", "正在收音"},
        {"LIVE CHATTING", "畅聊进行中"},
        {"LIVE LISTENING", "正在聆听"},
        {"RECORDING TOO LONG", "说话时间过长"},
        {"HOLD TO TALK", "按住就能说"},
        {"THINKING", "正在思考"},
        {"VOICE UNAVAILABLE", "语音暂不可用"},
        {"CHAT ENDED", "对话已结束"},
        {"CHAT ACTIVE", "畅聊已开启"},
        {"SERVER NOT CONNECTED", "服务未连接"},
        {"NEW CONVERSATION", "正在新建对话"},
        {"SETUP UNAVAILABLE", "配网暂不可用"},
        {"STOPPED", "已停止回复"},
        {"WI-FI RECONNECTING", "正在重连网络"},
        {"WI-FI SAVED", "网络已保存"},
        {"CONNECTING WI-FI", "正在连接网络"},
        {"END CHAT FIRST", "请先结束对话"},
        {"UPDATING", "正在更新固件"},
        {"UPDATE FAILED", "更新失败"},
        {"UPDATE COMPLETE", "更新完成"},
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0e1720), 0);
    lv_obj_set_style_text_font(screen, &ui_font_18, 0);
    lv_obj_t *title = label(16, 14, 104, 22, 0xc1d0d6);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "GPT LIVE");
    mode_label = label(120, 10, 104, 26, 0x83e2c6);
    lv_obj_set_style_text_align(mode_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(mode_label, "按住说话");
    battery_label = label(16, 43, 116, 26, 0xeaf1f3);
    volume_label = label(138, 43, 86, 26, 0xeaf1f3);
    lv_obj_set_style_text_align(volume_label, LV_TEXT_ALIGN_RIGHT, 0);
    panel(16, 75, 208, 1, 0x2a3b47, 0);
    status_label = label(16, 86, 208, 30, 0x83e2c6);
    lv_obj_set_style_text_font(status_label, &ui_font_22, 0);
    selection = panel(10, 119, 220, 25, 0x234a49, 5);
    lv_obj_set_hidden(selection, true);
    guide = label(18, 137, 208, 120, 0xeaf1f3);
    detail_label = label(16, 269, 208, 46, 0x9fb2bf);
    lv_obj_set_style_text_line_space(detail_label, 2, 0);
    volume_popup = label(48, 179, 144, 44, 0x83e2c6);
    lv_obj_set_style_text_align(volume_popup, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(volume_popup, 10, 0);
    lv_obj_set_style_bg_color(volume_popup, lv_color_hex(0x253e4b), 0);
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(volume_popup, 8, 0);
    lv_obj_set_hidden(volume_popup, true);
    last_touch = esp_timer_get_time();
    bsp_lvgl_unlock();
    ui_show_guide();
    ui_battery(-1, settings_get().volume);
    ui_set("STARTING", "正在准备声音与网络");
}

void ui_set(const char *state, const char *detail)
{
    if (!status_label || !bsp_lvgl_lock(100))
        return;
    // Keep menus and QR pages stable while background network events arrive.
    if (!qr && !menu_visible) {
        const char *text = state_text(state);
        if (strcmp(lv_label_get_text(status_label), text))
            lv_label_set_text(status_label, text);
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
            lv_label_set_text_fmt(battery_label, "电量 %d%%", percent);
        else
            lv_label_set_text(battery_label, "电量 --");
        lv_obj_set_style_text_color(
            battery_label, lv_color_hex(percent >= 0 && percent <= 15 ? 0xffbd82 : 0xeaf1f3), 0);
        shown_battery = percent;
    }
    if (volume != shown_volume) {
        lv_label_set_text_fmt(volume_label, "音量 %d", volume);
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
    lv_obj_set_hidden(selection, true);
    setup_visible = setup;
    lv_obj_set_hidden(guide, true);
    lv_label_set_text(mode_label, setup ? "网络配置" : "手机设置");
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
    if (lv_qrcode_update(qr, payload, strlen(payload)) != LV_RESULT_OK) {
        lv_label_set_text(status_label, "二维码生成失败");
        lv_label_set_text(detail_label, "请返回后重试");
    }
    bsp_lvgl_unlock();
}

void ui_show_setup(const char *ssid, bool website)
{
    char payload[96], detail[128];
    if (website) {
        show_qr("第二步：扫码配网", "http://192.168.4.1/", "短按 OK 切换二维码\n长按 OK 重试原网络",
                true);
    } else {
        snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
        snprintf(detail, sizeof(detail), "%s\n连上后按 OK 进入配网", ssid);
        show_qr("第一步：连接热点", payload, detail, true);
    }
}

void ui_show_settings(const char *url)
{
    show_qr("扫码打开设置", url, "手机连接同一网络\n长按 OK 返回", false);
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
    lv_obj_set_hidden(selection, true);
    lv_label_set_text(mode_label, live_mode ? "自由畅聊" : "按住说话");
    lv_obj_set_pos(guide, 18, 137);
    lv_obj_set_size(guide, 208, 120);
    lv_obj_set_style_text_line_space(guide, 30 - ui_font_18.line_height, 0);
    lv_label_set_text(
        guide,
        live_mode
            ? "直接说话，自由畅聊\n轻按 OK 停止回复\n按住 OK 切回按住说话\n上 / 下键调节音量"
            : "按住 OK 说话，松开发送\n轻按 OK 停止回复\n上 / 下键调节音量\n下键 + OK 打开菜单");
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
    lv_label_set_text(mode_label, "快捷设置");
    lv_obj_set_hidden(guide, false);
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
    lv_obj_set_y(selection, 119 + (selected - first) * 25);
    lv_obj_set_hidden(selection, false);
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
    lv_label_set_text_fmt(volume_popup, "音量  %d", volume);
    lv_obj_set_hidden(volume_popup, false);
    lv_obj_move_foreground(volume_popup);
    bsp_lvgl_unlock();
}
