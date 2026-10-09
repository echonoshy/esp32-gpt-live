// Compile with menu.c and a host stub esp_err.h defining ESP_OK and esp_err_t.
#include "menu.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static settings_t prefs = {.volume = 70, .brightness = 65, .screen_seconds = 60,
                          .idle_seconds = 180, .duplex = 1};
static char title[64];
static int focus, row_count;
static char rows[10][64];
settings_t settings_get(void)
{
    return prefs;
}
esp_err_t settings_save(settings_t p)
{
    prefs = p;
    return ESP_OK;
}
const char *settings_voice(int i)
{
    (void)i;
    return "cove";
}
bool voice_wifi_connected(void)
{
    return true;
}
void voice_wifi_ip(char *s, size_t n)
{
    snprintf(s, n, "192.0.2.1");
}
void ui_show_menu(const char *t, const char *const *items, int count, int selected,
                  const char *hint)
{
    assert(selected >= 0 && selected < count);
    assert(strstr(hint, "长按 OK 返回上一级"));
    snprintf(title, sizeof(title), "%s", t);
    focus = selected;
    row_count = count;
    for (int i = 0; i < count; i++) {
        assert(strcmp(items[i], "结束对话"));
        snprintf(rows[i], sizeof(rows[i]), "%s", items[i]);
    }
}
int main(void)
{
    menu_open(false);
    assert(row_count == 6 && !strcmp(rows[1], "重置上下文"));
    assert(menu_key(0) == MENU_LIVE);
    assert(menu_back() == MENU_CLOSE);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE); // Entering confirmation must not reset context.
    assert(!strcmp(title, "重置上下文？"));
    assert(menu_back() == MENU_NONE && !strcmp(title, "快捷菜单") && focus == 1);
    menu_key(0);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "快捷菜单")); // Cancel row.
    menu_key(0);
    assert(menu_key(0) == MENU_NEW_CHAT); // Only explicit confirmation resets.
    menu_back();
    menu_key(1);
    menu_key(0); // Root opens voice selection directly.
    assert(!strcmp(title, "选择音色"));
    menu_key(1);
    menu_key(0); // Save the selected voice and return to root.
    assert(prefs.voice == 1 && !strcmp(title, "快捷菜单"));
    menu_key(0); // Focus stays on voice; reopen at the saved choice.
    assert(!strcmp(title, "选择音色"));
    menu_key(1);
    menu_back();
    assert(prefs.voice == 1 && !strcmp(title, "快捷菜单"));
    // The explicit back row also returns to root without changing the voice.
    menu_key(0);
    menu_key(-1);
    menu_key(-1);
    menu_key(0);
    assert(prefs.voice == 1 && !strcmp(title, "快捷菜单"));
    menu_key(1);
    menu_key(0); // Device
    menu_key(0);
    menu_key(-1); // Volume +5 saved before returning
    menu_back();
    assert(!strcmp(title, "设备设置") && prefs.volume == 75);
    for (int i = 0; i < 3; i++)
        menu_key(1);
    assert(strstr(rows[focus], "空闲结束"));
    menu_key(0);
    menu_key(-1);
    menu_key(0);
    assert(!strcmp(title, "设备设置") && focus == 3 && prefs.idle_seconds == 300);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "进入手机设置？"));
    assert(menu_back() == MENU_NONE && !strcmp(title, "设备设置") && focus == 4);
    menu_key(0);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "设备设置") && focus == 4);
    menu_key(0);
    assert(menu_key(0) == MENU_PHONE); // Maintenance also requires explicit confirmation.
    menu_refresh();
    assert(!strcmp(title, "设备设置") && focus == 4); // QR exit returns here.
    menu_back();
    menu_key(1);
    menu_key(1);
    menu_key(1);
    menu_key(1);
    menu_key(0); // Network
    menu_key(0);
    assert(!strcmp(title, "重新配置网络？"));
    menu_back();
    assert(!strcmp(title, "网络设置"));
    menu_key(1);
    menu_key(0);
    assert(!strcmp(title, "连接信息"));
    menu_back();
    assert(!strcmp(title, "网络设置"));
    menu_back();
    assert(menu_back() == MENU_CLOSE);
    puts("PASS: direct context reset, reset/maintenance confirmation and cancellation, idle setting, voice and menu navigation");
}
