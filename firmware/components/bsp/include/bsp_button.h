#pragma once
#include "esp_err.h"
#include <stdint.h>
// Shared resistor ladder. DOWN+OK is a distinct voltage, not two GPIO interrupts.
enum {
    BSP_KEY_NONE = 0,
    BSP_KEY_UP = 1,
    BSP_KEY_DOWN = 2,
    BSP_KEY_OK = 4,
    BSP_KEY_MENU = 6,
    BSP_KEY_INVALID = 8
};
typedef void (*bsp_btn_cb_t)(uint8_t keys, void *user);
// Callback runs every 10 ms on the ESP timer task with a debounced mask. Never block it.
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);
int bsp_button_read_mv(void);
uint8_t bsp_button_read_keys(void);
