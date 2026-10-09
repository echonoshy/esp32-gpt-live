#pragma once
#include <stdbool.h>
typedef enum {
    MENU_NONE,
    MENU_CLOSE,
    MENU_LIVE,
    MENU_NEW_CHAT,
    MENU_VOICE_CHANGED,
    MENU_WIFI,
    MENU_PHONE
} menu_action_t;
void menu_open(bool live);
// -1 up, +1 down, 0 select. All state belongs to the command task.
menu_action_t menu_key(int key);
menu_action_t menu_back(void);
void menu_refresh(void);
