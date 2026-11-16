#pragma once
#include <stdint.h>
#define AUDIO_RATE 32728
void audio_init(void);                              /* after settings_load; uses SDL audio unless headless */
void audio_push(const int16_t *stereo, int frames); /* interleaved stereo frames at AUDIO_RATE */
