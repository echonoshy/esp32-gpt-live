#pragma once
#include "esp_err.h"
#define VOICE_COUNT 9
typedef struct {
    int volume, brightness, screen_seconds, idle_seconds;
    int mode, voice, pace, reply_length, language, duplex;
} settings_t;
void settings_init(void);
settings_t settings_get(void);
esp_err_t settings_save(settings_t value);
const char *settings_voice(int index);
