#include "adpcm.h"
static const int steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int indices[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static int clamp(int v, int low, int high)
{
    return v < low ? low : v > high ? high : v;
}
static int sample(adpcm_state_t *s, unsigned code)
{
    int step = steps[s->index];
    int delta = step >> 3;
    if (code & 4)
        delta += step;
    if (code & 2)
        delta += step >> 1;
    if (code & 1)
        delta += step >> 2;
    s->predictor = clamp(s->predictor + ((code & 8) ? -delta : delta), -32768, 32767);
    s->index = clamp(s->index + indices[code & 7], 0, 88);
    return s->predictor;
}
void adpcm_encode(adpcm_state_t *s, const int16_t pcm[ADPCM_SAMPLES], uint8_t out[ADPCM_BYTES])
{
    out[0] = (uint16_t)s->predictor & 255;
    out[1] = (uint16_t)s->predictor >> 8;
    out[2] = s->index;
    out[3] = 0;
    for (unsigned i = 0; i < ADPCM_SAMPLES; i++) {
        int diff = pcm[i] - s->predictor;
        unsigned code = diff < 0 ? 8 : 0;
        if (diff < 0)
            diff = -diff;
        int step = steps[s->index];
        if (diff >= step) {
            code |= 4;
            diff -= step;
        }
        if (diff >= (step >> 1)) {
            code |= 2;
            diff -= step >> 1;
        }
        if (diff >= (step >> 2))
            code |= 1;
        sample(s, code);
        if (i % 2 == 0)
            out[4 + i / 2] = code << 4;
        else
            out[4 + i / 2] |= code;
    }
}
bool adpcm_decode(const uint8_t *p, size_t size, int16_t pcm[ADPCM_SAMPLES])
{
    if (size != ADPCM_BYTES || p[2] > 88 || p[3] != 0)
        return false;
    adpcm_state_t s = {.predictor = (int16_t)(p[0] | p[1] << 8), .index = p[2]};
    for (unsigned i = 0; i < ADPCM_SAMPLES; i++)
        pcm[i] = sample(&s, (i % 2 == 0 ? p[4 + i / 2] >> 4 : p[4 + i / 2]) & 15);
    return true;
}
