// Compile with menu.c and a host stub esp_err.h defining ESP_OK and esp_err_t.
#include "menu.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static settings_t prefs = {70, 65, 60, 180, 0, 0, 0, 0, 0, 0};
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
    assert(strstr(hint, "Hold OK: back"));
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
    assert(!strcmp(title, "CLEAR CONTEXT?"));
    assert(menu_back() == MENU_NONE && !strcmp(title, "CONVERSATION"));
    menu_back();
    menu_key(1);
    menu_key(1);
    menu_key(0);
    menu_key(0); // Voice > Choose voice
    assert(!strcmp(title, "CHOOSE VOICE"));
    menu_back();
    assert(!strcmp(title, "VOICE"));
    menu_back();
    menu_key(1);
    menu_key(1);
    menu_key(1);
    menu_key(0); // Device
    menu_key(0);
    menu_key(-1); // Volume +5 saved before returning
    menu_back();
    assert(!strcmp(title, "DEVICE") && prefs.volume == 75);
    menu_back();
    menu_key(1);
    menu_key(1);
    menu_key(1);
    menu_key(1);
    menu_key(0); // Network
    menu_key(0);
    assert(!strcmp(title, "SET UP WI-FI?"));
    menu_back();
    assert(!strcmp(title, "NETWORK"));
    menu_key(1);
    menu_key(0);
    assert(!strcmp(title, "CONNECTION"));
    menu_back();
    assert(!strcmp(title, "NETWORK"));
    menu_back();
    assert(menu_back() == MENU_CLOSE);
    puts("PASS: every menu parent, confirmation back, volume retention, root exit");
}
