#pragma once
#include <stdint.h>
// Nominal ladder: UP 0 mV, DOWN+OK 212 mV, DOWN 300 mV, OK 595 mV.
// Guard bands reject ambiguous readings instead of treating them as a menu chord.
static inline uint8_t bsp_button_decode_mv(int mv)
{
    if (mv < 0)
        return 8;
    if (mv < 150)
        return 1;
    if (mv >= 175 && mv <= 250)
        return 6;
    if (mv >= 270 && mv < 447)
        return 2;
    if (mv >= 470 && mv < 1900)
        return 4;
    if (mv >= 1900)
        return 0;
    return 8;
}
