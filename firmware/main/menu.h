#pragma once
#include <stdbool.h>
typedef enum {
    MENU_NONE,
    MENU_CHAT,
    MENU_LIVE,
    MENU_NEW_CHAT,
    MENU_VOICE_CHANGED,
    MENU_WIFI,
    MENU_PHONE
} menu_action_t;
// Open the three-entry home page, remembering the returning mode's selection.
void menu_open(int selection);
// -1 up, +1 down, 0 select. All state belongs to the command task.
menu_action_t menu_key(int key);
menu_action_t menu_back(void);
void menu_refresh(void);
