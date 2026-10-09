#pragma once
#include "esp_err.h"
#define VOICE_COUNT 9
typedef struct {
    int volume, brightness, screen_seconds, idle_seconds;
    int mode, voice;
    int reserved_style[3]; // Retired pace/length/language slots retain prefs_v4 offsets.
    int duplex;
} settings_t;
_Static_assert(sizeof(settings_t) == 40, "preserve prefs_v4 NVS layout");
void settings_init(void);
settings_t settings_get(void);
esp_err_t settings_save(settings_t value);
const char *settings_voice(int index);

// Apply while held; persist once when the volume gesture ends.
void settings_adjust_volume(int delta);
