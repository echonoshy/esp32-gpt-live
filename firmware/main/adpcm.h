#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ADPCM_SAMPLES 480
#define ADPCM_BYTES (4 + ADPCM_SAMPLES / 2)
typedef struct {
    int predictor, index;
} adpcm_state_t;
// Each packet carries its initial decoder state. Nibbles are high-first (IMA/DVI).
void adpcm_encode(adpcm_state_t *state, const int16_t pcm[ADPCM_SAMPLES], uint8_t out[ADPCM_BYTES]);
bool adpcm_decode(const uint8_t *packet, size_t size, int16_t pcm[ADPCM_SAMPLES]);
