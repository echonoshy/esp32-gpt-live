#include "menu.h"
#include "settings.h"
#include "ui.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    ROOT,
    CHAT,
    VOICE,
    DEVICE,
    NETWORK,
    PICK_VOICE,
    EDIT_VOLUME,
    EDIT_BRIGHT,
    EDIT_SCREEN,
    EDIT_IDLE,
    CONFIRM_NEW,
    CONFIRM_WIFI,
    INFO
} page_t;
static page_t page;
static int selected;
static const char *const pace[] = {"Normal", "Slow", "Fast"};
static const char *const length[] = {"Short", "One sentence", "Detailed"};
static const char *const language[] = {"Chinese", "English", "Follow user"};
static const int sleeps[] = {15, 30, 60, 120, 300, 600};
static const int timeouts[] = {60, 180, 300, 600, 900};
static int count(void)
{
    switch (page) {
    case ROOT:
    case VOICE:
    case DEVICE:
    case CHAT:
        return 5;
    case NETWORK:
        return 3;
    case PICK_VOICE:
        return VOICE_COUNT + 1;
    case CONFIRM_NEW:
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
    settings_t p = settings_get();
    char rows[10][42] = {{0}}, title[32] = "SETTINGS", hint[96] = "UP/DOWN: move  OK: select";
    switch (page) {
    case ROOT:
        snprintf(rows[0], 42, "Conversation");
        snprintf(rows[1], 42, "Voice");
        snprintf(rows[2], 42, "Device");
        snprintf(rows[3], 42, "Network");
        snprintf(rows[4], 42, "Back to chat");
        break;
    case CHAT:
        snprintf(title, 32, "CONVERSATION");
        snprintf(rows[0], 42, "New chat (clear context)");
        snprintf(rows[1], 42, "End chat");
        snprintf(rows[2], 42, "Live duplex: %s", p.duplex ? "Full" : "Half");
        snprintf(rows[3], 42, "Idle timeout: %d s", p.idle_seconds);
        snprintf(rows[4], 42, "Back");
        snprintf(hint, 96, "Duplex: next new chat");
        break;
    case VOICE:
        snprintf(title, 32, "VOICE");
        snprintf(rows[0], 42, "Voice: %s", settings_voice(p.voice));
        snprintf(rows[1], 42, "Pace: %s", pace[p.pace]);
        snprintf(rows[2], 42, "Reply: %s", length[p.reply_length]);
        snprintf(rows[3], 42, "Language: %s", language[p.language]);
        snprintf(rows[4], 42, "Back");
        snprintf(hint, 96, "Saved for next new chat\nPace/reply/language: guidance");
        break;
    case DEVICE:
        snprintf(title, 32, "DEVICE");
        snprintf(rows[0], 42, "Volume: %d", p.volume);
        snprintf(rows[1], 42, "Brightness: %d", p.brightness);
        snprintf(rows[2], 42, "Screen sleep: %d s", p.screen_seconds);
        snprintf(rows[3], 42, "Phone / firmware update");
        snprintf(rows[4], 42, "Back");
        break;
    case NETWORK:
        snprintf(title, 32, "NETWORK");
        snprintf(rows[0], 42, "Wi-Fi setup (QR)");
        snprintf(rows[1], 42, "Connection info");
        snprintf(rows[2], 42, "Back");
        break;
    case PICK_VOICE:
        snprintf(title, 32, "CHOOSE VOICE");
        for (int i = 0; i < VOICE_COUNT; i++)
            snprintf(rows[i], 42, "%s%s", i == p.voice ? "* " : "", settings_voice(i));
        snprintf(rows[VOICE_COUNT], 42, "Back");
        snprintf(hint, 96, "OK: save this voice\nApplies to next new chat");
        break;
    case CONFIRM_NEW:
        snprintf(title, 32, "CLEAR CONTEXT?");
        snprintf(rows[0], 42, "Yes, start a new chat");
        snprintf(rows[1], 42, "Back");
        snprintf(hint, 96, "Current conversation ends\nNew voice settings will apply");
        break;
    case CONFIRM_WIFI:
        snprintf(title, 32, "SET UP WI-FI?");
        snprintf(rows[0], 42, "Restart into setup");
        snprintf(rows[1], 42, "Back");
        snprintf(hint, 96, "Chat ends; saved Wi-Fi kept\nScan QR with your phone");
        break;
    case INFO: {
        char ip[20];
        voice_wifi_ip(ip, sizeof(ip));
        snprintf(title, 32, "CONNECTION");
        snprintf(rows[0], 42, "%s", voice_wifi_connected() ? ip : "Wi-Fi disconnected");
        snprintf(hint, 96, "OK: back");
        break;
    }
    default:
        snprintf(title, 32, "ADJUST");
        snprintf(rows[0], 42, "%s: %d",
                 page == EDIT_VOLUME   ? "Volume"
                 : page == EDIT_BRIGHT ? "Brightness"
                 : page == EDIT_SCREEN ? "Sleep (s)"
                                       : "Idle (s)",
                 page == EDIT_VOLUME   ? p.volume
                 : page == EDIT_BRIGHT ? p.brightness
                 : page == EDIT_SCREEN ? p.screen_seconds
                                       : p.idle_seconds);
        snprintf(hint, 96, "UP/DOWN: adjust\nOK: save and back");
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
void menu_open(void)
{
    enter(ROOT);
    menu_refresh();
}
menu_action_t menu_back(void)
{
    switch (page) {
    case ROOT:
        return MENU_CLOSE;
    case PICK_VOICE:
        enter(VOICE);
        break;
    case EDIT_VOLUME:
    case EDIT_BRIGHT:
    case EDIT_SCREEN:
        enter(DEVICE);
        break;
    case EDIT_IDLE:
    case CONFIRM_NEW:
        enter(CHAT);
        break;
    case CONFIRM_WIFI:
    case INFO:
        enter(NETWORK);
        break;
    default:
        enter(ROOT);
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
        case ROOT:
            if (selected == 4)
                return MENU_CLOSE;
            enter((page_t[]){CHAT, VOICE, DEVICE, NETWORK}[selected]);
            break;
        case CHAT:
            if (selected == 0)
                enter(CONFIRM_NEW);
            else if (selected == 1)
                return MENU_STOP;
            else if (selected == 2) {
                p.duplex = !p.duplex;
                changed = true;
            } else if (selected == 3)
                enter(EDIT_IDLE);
            else
                enter(ROOT);
            break;
        case VOICE:
            if (selected == 0) {
                enter(PICK_VOICE);
                selected = p.voice;
            } else if (selected == 1) {
                p.pace = (p.pace + 1) % 3;
                changed = true;
            } else if (selected == 2) {
                p.reply_length = (p.reply_length + 1) % 3;
                changed = true;
            } else if (selected == 3) {
                p.language = (p.language + 1) % 3;
                changed = true;
            } else
                enter(ROOT);
            break;
        case DEVICE:
            if (selected < 3)
                enter((page_t[]){EDIT_VOLUME, EDIT_BRIGHT, EDIT_SCREEN}[selected]);
            else if (selected == 3)
                return MENU_PHONE;
            else
                enter(ROOT);
            break;
        case NETWORK:
            enter((page_t[]){CONFIRM_WIFI, INFO, ROOT}[selected]);
            break;
        case PICK_VOICE:
            if (selected < VOICE_COUNT) {
                p.voice = selected;
                changed = true;
            }
            enter(VOICE);
            break;
        case CONFIRM_NEW:
            if (selected == 0)
                return MENU_NEW_CHAT;
            enter(CHAT);
            break;
        case CONFIRM_WIFI:
            if (selected == 0)
                return MENU_WIFI;
            enter(NETWORK);
            break;
        case INFO:
            enter(NETWORK);
            break;
        case EDIT_IDLE:
            enter(CHAT);
            break;
        default:
            enter(DEVICE);
            break;
        }
    if (changed && settings_save(p) != ESP_OK) {
        const char *message[] = {"Could not save settings", "Press OK to retry"};
        ui_show_menu("SAVE FAILED", message, 2, 0, "Settings unchanged");
        return MENU_NONE;
    }
    menu_refresh();
    return MENU_NONE;
}
