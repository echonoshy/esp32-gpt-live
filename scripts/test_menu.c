// Compile with menu.c and a host stub esp_err.h defining ESP_OK and esp_err_t.
#include "menu.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static settings_t prefs = {.volume = 70, .brightness = 65, .screen_seconds = 60,
                          .idle_seconds = 180, .duplex = 1};
static char title[64];
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
    (void)items;
    assert(selected >= 0 && selected < count);
    assert(strstr(hint, "长按 OK 返回上一级"));
    snprintf(title, sizeof(title), "%s", t);
}
int main(void)
{
    menu_open(false);
    assert(menu_key(0) == MENU_LIVE);
    assert(menu_back() == MENU_CLOSE);
    menu_key(1);
    menu_key(0); // Conversation
    menu_key(0); // New chat confirmation
    assert(!strcmp(title, "清空上下文？"));
    assert(menu_back() == MENU_NONE && !strcmp(title, "对话管理"));
    menu_back();
    menu_key(1);
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
    puts("PASS: direct voice selection/save/cancel/back, every menu parent, volume retention, root exit");
}
