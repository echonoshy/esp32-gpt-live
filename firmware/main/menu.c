#include "menu.h"
#include "settings.h"
#include "ui.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    HOME,
    ROOT,
    DEVICE,
    NETWORK,
    PICK_VOICE,
    EDIT_VOLUME,
    EDIT_BRIGHT,
    EDIT_SCREEN,
    EDIT_IDLE,
    CONFIRM_NEW,
    CONFIRM_UPDATE,
    CONFIRM_WIFI,
    INFO
} page_t;
static page_t page;
static int selected;
static const int sleeps[] = {15, 30, 60, 120, 300, 600};
static const int timeouts[] = {60, 180, 300, 600, 900};
static int count(void)
{
    switch (page) {
    case HOME:
        return 3;
    case ROOT:
        return 5;
    case DEVICE:
        return 6;
    case NETWORK:
        return 3;
    case PICK_VOICE:
        return VOICE_COUNT + 1;
    case CONFIRM_NEW:
    case CONFIRM_UPDATE:
    case CONFIRM_WIFI:
        return 2;
    default:
        return 1;
    }
}
static void enter(page_t next)
{
    page = next;
    selected = 0;
}
static bool editing(void)
{
    return page >= EDIT_VOLUME && page <= EDIT_IDLE;
}
void menu_refresh(void)
{
    if (page == HOME) {
        ui_show_home(selected);
        return;
    }
    settings_t p = settings_get();
    char rows[10][64] = {{0}}, title[48] = "SETTINGS", hint[128] = "UP/DN: select / OK";
    switch (page) {
    case ROOT:
        snprintf(rows[0], sizeof(rows[0]), "Voice: %s", settings_voice(p.voice));
        snprintf(rows[1], sizeof(rows[1]), "Device");
        snprintf(rows[2], sizeof(rows[2]), "Network");
        snprintf(rows[3], sizeof(rows[3]), "Reset context");
        snprintf(rows[4], sizeof(rows[4]), "Back to home");
        break;
    case DEVICE:
        snprintf(title, sizeof(title), "DEVICE");
        snprintf(rows[0], sizeof(rows[0]), "Volume: %d", p.volume);
        snprintf(rows[1], sizeof(rows[1]), "Brightness: %d", p.brightness);
        snprintf(rows[2], sizeof(rows[2]), "Screen off: %d s", p.screen_seconds);
        snprintf(rows[3], sizeof(rows[3]), "Idle end: %d s", p.idle_seconds);
        snprintf(rows[4], sizeof(rows[4]), "Firmware update");
        snprintf(rows[5], sizeof(rows[5]), "Back");
        break;
    case NETWORK:
        snprintf(title, sizeof(title), "NETWORK");
        snprintf(rows[0], sizeof(rows[0]), "Connect Wi-Fi");
        snprintf(rows[1], sizeof(rows[1]), "Connection info");
        snprintf(rows[2], sizeof(rows[2]), "Back");
        break;
    case PICK_VOICE:
        snprintf(title, sizeof(title), "VOICE SELECT");
        for (int i = 0; i < VOICE_COUNT; i++)
            snprintf(rows[i], sizeof(rows[i]), "%s%s", i == p.voice ? "* " : "", settings_voice(i));
        snprintf(rows[VOICE_COUNT], sizeof(rows[VOICE_COUNT]), "Back");
        snprintf(hint, sizeof(hint), "Saved / next chat uses voice");
        break;
    case CONFIRM_NEW:
        snprintf(title, sizeof(title), "RESET CONTEXT?");
        snprintf(rows[0], sizeof(rows[0]), "Clear and restart");
        snprintf(rows[1], sizeof(rows[1]), "Back");
        snprintf(hint, sizeof(hint), "Clears this session");
        break;
    case CONFIRM_UPDATE:
        snprintf(title, sizeof(title), "UPDATE FIRMWARE?");
        snprintf(rows[0], sizeof(rows[0]), "Check / install latest");
        snprintf(rows[1], sizeof(rows[1]), "Back");
        snprintf(hint, sizeof(hint), "Ends session / keep power on");
        break;
    case CONFIRM_WIFI:
        snprintf(title, sizeof(title), "WI-FI SETUP?");
        snprintf(rows[0], sizeof(rows[0]), "Restart into setup");
        snprintf(rows[1], sizeof(rows[1]), "Back");
        snprintf(hint, sizeof(hint), "Keeps saved Wi-Fi");
        break;
    case INFO: {
        char ip[20];
        voice_wifi_ip(ip, sizeof(ip));
        snprintf(title, sizeof(title), "CONNECTION");
        snprintf(rows[0], sizeof(rows[0]), "%s", voice_wifi_connected() ? ip : "Wi-Fi offline");
        snprintf(hint, sizeof(hint), "OK: back");
        break;
    }
    default:
        snprintf(title, sizeof(title), "ADJUST");
        snprintf(rows[0], sizeof(rows[0]), "%s: %d",
                 page == EDIT_VOLUME   ? "Volume"
                 : page == EDIT_BRIGHT ? "Brightness"
                 : page == EDIT_SCREEN ? "Screen off (s)"
                                       : "Idle end (s)",
                 page == EDIT_VOLUME   ? p.volume
                 : page == EDIT_BRIGHT ? p.brightness
                 : page == EDIT_SCREEN ? p.screen_seconds
                                       : p.idle_seconds);
        snprintf(hint, sizeof(hint), "UP/DN: adjust / OK");
        break;
    }
    const char *items[10];
    // Keep the back gesture visible on every level, including edit/confirmation pages.
    char *line = strchr(hint, '\n');
    if (line)
        *line = 0;
    strncat(hint, "\nHold OK: back", sizeof(hint) - strlen(hint) - 1);
    for (int i = 0; i < 10; i++)
        items[i] = rows[i];
    ui_show_menu(title, items, count(), selected, hint);
}
void menu_open(int selection)
{
    enter(HOME);
    selected = selection >= 0 && selection < 3 ? selection : 0;
    menu_refresh();
}
menu_action_t menu_back(void)
{
    switch (page) {
    case HOME:
        return MENU_NONE;
    case ROOT:
        enter(HOME);
        selected = 2;
        break;
    case PICK_VOICE:
        enter(ROOT);
        break;
    case EDIT_VOLUME:
    case EDIT_BRIGHT:
    case EDIT_SCREEN:
    case EDIT_IDLE:
        selected = page - EDIT_VOLUME;
        page = DEVICE;
        break;
    case CONFIRM_NEW:
        enter(ROOT);
        selected = 3;
        break;
    case CONFIRM_UPDATE:
        enter(DEVICE);
        selected = 4;
        break;
    case CONFIRM_WIFI:
    case INFO:
        enter(NETWORK);
        break;
    case DEVICE:
        enter(ROOT);
        selected = 1;
        break;
    case NETWORK:
        enter(ROOT);
        selected = 2;
        break;
    default:
        break;
    }
    menu_refresh();
    return MENU_NONE;
}
static int cycle(int v, const int *values, int n, int direction)
{
    int i = 0;
    while (i < n - 1 && values[i] < v)
        i++;
    return values[(i + direction + n) % n];
}
menu_action_t menu_key(int key)
{
    settings_t p = settings_get();
    bool changed = false;
    if (key && editing()) {
        int delta = key < 0 ? 1 : -1;
        if (page == EDIT_VOLUME) {
            p.volume += delta * 5;
            if (p.volume < 0)
                p.volume = 0;
            if (p.volume > 100)
                p.volume = 100;
        }
        if (page == EDIT_BRIGHT) {
            p.brightness += delta * 5;
            if (p.brightness < 5)
                p.brightness = 5;
            if (p.brightness > 100)
                p.brightness = 100;
        }
        if (page == EDIT_SCREEN)
            p.screen_seconds = cycle(p.screen_seconds, sleeps, 6, delta);
        if (page == EDIT_IDLE)
            p.idle_seconds = cycle(p.idle_seconds, timeouts, 5, delta);
        changed = true;
    } else if (key)
        selected = (selected + key + count()) % count();
    else
        switch (page) {
        case HOME:
            if (selected == 0)
                return MENU_CHAT;
            if (selected == 1)
                return MENU_LIVE;
            enter(ROOT);
            break;
        case ROOT:
            if (selected == 4)
                return menu_back();
            enter((page_t[]){PICK_VOICE, DEVICE, NETWORK, CONFIRM_NEW}[selected]);
            if (page == PICK_VOICE)
                selected = p.voice;
            break;
        case DEVICE:
            if (selected < 4)
                enter((page_t[]){EDIT_VOLUME, EDIT_BRIGHT, EDIT_SCREEN, EDIT_IDLE}[selected]);
            else if (selected == 4)
                enter(CONFIRM_UPDATE);
            else
                return menu_back();
            break;
        case NETWORK:
            if (selected == 2)
                return menu_back();
            enter((page_t[]){CONFIRM_WIFI, INFO}[selected]);
            break;
        case PICK_VOICE:
            if (selected == VOICE_COUNT)
                return menu_back();
            if (selected == p.voice) {
                menu_refresh();
                return MENU_NONE;
            }
            p.voice = selected;
            if (settings_save(p) != ESP_OK) {
                const char *message[] = {"Could not save", "OK: retry"};
                ui_show_menu("SAVE FAILED", message, 2, 0, "Hold OK: back");
                return MENU_NONE;
            }
            // Saving a preference never navigates away from its picker.
            menu_refresh();
            return MENU_VOICE_CHANGED;
        case CONFIRM_NEW:
            if (selected == 0)
                return MENU_NEW_CHAT;
            return menu_back();
        case CONFIRM_UPDATE:
            if (selected == 0) {
                enter(DEVICE); selected = 4; return MENU_UPDATE;
            }
            return menu_back();
        case CONFIRM_WIFI:
            if (selected == 0)
                return MENU_WIFI;
            enter(NETWORK);
            break;
        case INFO:
            enter(NETWORK);
            break;
        default:
            return menu_back();
        }
    if (changed && settings_save(p) != ESP_OK) {
        const char *message[] = {"Could not save", "OK: retry"};
        ui_show_menu("SAVE FAILED", message, 2, 0, "Settings unchanged");
        return MENU_NONE;
    }
    menu_refresh();
    return MENU_NONE;
}
