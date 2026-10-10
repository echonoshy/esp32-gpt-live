// Compile with menu.c and a host stub esp_err.h defining ESP_OK and esp_err_t.
#include "menu.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static settings_t prefs = {
    .volume = 70, .brightness = 65, .screen_seconds = 60, .idle_seconds = 180, .duplex = 1};
static char title[64];
static int focus, row_count;
static char rows[10][64];
static bool save_fails;
static int saves;
settings_t settings_get(void)
{
    return prefs;
}
esp_err_t settings_save(settings_t p)
{
    saves++;
    if (save_fails)
        return -1;
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
    assert(strstr(hint, "Hold OK: back"));
    snprintf(title, sizeof(title), "%s", t);
    focus = selected;
    row_count = count;
    for (int i = 0; i < count; i++) {
        assert(strcmp(items[i], "End session"));
        snprintf(rows[i], sizeof(rows[i]), "%s", items[i]);
    }
}
void ui_show_home(int selected)
{
    assert(selected >= 0 && selected < 3);
    snprintf(title, sizeof(title), "HOME");
    focus = selected;
    row_count = 3;
}
static void settings_root(void)
{
    menu_open(2);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "SETTINGS"));
    assert(row_count == 5 && focus == 0);
}
int main(void)
{
    menu_open(0);
    assert(!strcmp(title, "HOME") && row_count == 3 && focus == 0);
    assert(menu_back() == MENU_NONE && focus == 0);
    assert(menu_key(0) == MENU_CHAT);
    menu_key(1);
    assert(menu_key(0) == MENU_LIVE);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "SETTINGS"));
    assert(menu_back() == MENU_NONE && !strcmp(title, "HOME") && focus == 2);
    menu_key(1);
    assert(focus == 0);
    menu_key(-1);
    assert(focus == 2);
    // Home always remembers which entry returned; an invalid selection is clamped.
    menu_open(1);
    assert(focus == 1 && menu_key(0) == MENU_LIVE);
    menu_open(99);
    assert(focus == 0);
    settings_root();
    menu_key(0);
    assert(!strcmp(title, "VOICE SELECT"));
    menu_key(1);
    assert(menu_key(0) == MENU_VOICE_CHANGED && prefs.voice == 1);
    assert(!strcmp(title, "VOICE SELECT") && focus == 1);
    assert(!strncmp(rows[1], "* ", 2) && strncmp(rows[0], "* ", 2));
    int saved = saves;
    assert(menu_key(0) == MENU_NONE && saves == saved);
    assert(!strcmp(title, "VOICE SELECT") && focus == 1);
    menu_key(1);
    menu_back();
    assert(prefs.voice == 1 && !strcmp(title, "SETTINGS"));
    menu_key(0);
    menu_key(-1);
    menu_key(-1);
    menu_key(0); // Explicit back row from voice list.
    assert(prefs.voice == 1 && !strcmp(title, "SETTINGS"));
    // Device values, focus and explicit/hold back have identical hierarchy.
    menu_key(1);
    menu_key(0);
    menu_key(0);
    menu_key(-1);
    menu_back();
    assert(!strcmp(title, "DEVICE") && prefs.volume == 75 && focus == 0);
    for (int i = 0; i < 3; i++)
        menu_key(1);
    menu_key(0);
    menu_key(-1);
    menu_key(0);
    assert(focus == 3 && prefs.idle_seconds == 300);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "OPEN WEB SETUP?"));
    assert(menu_back() == MENU_NONE && focus == 4);
    menu_key(0);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && focus == 4);
    menu_key(0);
    assert(menu_key(0) == MENU_PHONE);
    menu_refresh();
    assert(!strcmp(title, "DEVICE") && focus == 4);
    menu_back();
    assert(!strcmp(title, "SETTINGS") && focus == 1);
    menu_key(1);
    menu_key(0);
    menu_key(0);
    assert(!strcmp(title, "WI-FI SETUP?"));
    menu_back();
    menu_key(1);
    menu_key(0);
    assert(!strcmp(title, "CONNECTION"));
    menu_back();
    menu_back();
    assert(!strcmp(title, "SETTINGS") && focus == 2);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "RESET CONTEXT?"));
    menu_back();
    assert(focus == 3);
    menu_key(0);
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && focus == 3); // Cancel preserves context.
    menu_key(0);
    assert(menu_key(0) == MENU_NEW_CHAT); // Only explicit confirmation resets.
    menu_back();
    menu_key(1);
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "HOME") && focus == 2);
    settings_root();
    menu_key(0);
    menu_key(1);
    save_fails = true;
    assert(menu_key(0) == MENU_NONE && prefs.voice == 1 && !strcmp(title, "SAVE FAILED"));
    save_fails = false;
    assert(menu_key(0) == MENU_VOICE_CHANGED && prefs.voice == 2);
    assert(!strcmp(title, "VOICE SELECT") && focus == 2);
    assert(!strncmp(rows[2], "* ", 2));
    assert(menu_key(0) == MENU_NONE && !strcmp(title, "VOICE SELECT"));
    menu_key(1);
    assert(menu_back() == MENU_NONE && prefs.voice == 2 && !strcmp(title, "SETTINGS"));
    puts("PASS: three home entries, wrap/focus, settings hierarchy, voice save/retry, "
         "reset/maintenance confirmation");
}
