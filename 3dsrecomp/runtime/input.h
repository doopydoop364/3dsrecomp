/* Host input mapped to the 3DS: buttons, circle pad and touch screen. */
#pragma once
#include <stdint.h>

/* 3DS pad bits (HID pad state). */
enum {
    BTN_A = 1u << 0, BTN_B = 1u << 1, BTN_SELECT = 1u << 2, BTN_START = 1u << 3,
    BTN_RIGHT = 1u << 4, BTN_LEFT = 1u << 5, BTN_UP = 1u << 6, BTN_DOWN = 1u << 7,
    BTN_R = 1u << 8, BTN_L = 1u << 9, BTN_X = 1u << 10, BTN_Y = 1u << 11,
    BTN_ZL = 1u << 14, BTN_ZR = 1u << 15,     /* New 3DS: reported by ir:rst, not by HID */
    CIRCLE_RIGHT = 1u << 28, CIRCLE_LEFT = 1u << 29, CIRCLE_UP = 1u << 30, CIRCLE_DOWN = 1u << 31,
};

typedef struct {
    uint32_t buttons;          /* pad bits, without the circle-pad direction bits */
    int16_t cx, cy;            /* circle pad, -154..154 (up is positive y) */
    int16_t sx, sy;            /* New 3DS C-stick, -156..156 (up is positive y) */
    int touch;                 /* stylus down */
    uint16_t tx, ty;           /* touch position on the bottom screen, 0..319 / 0..239 */
} InputState;

void input_init(void);
/* Called once per frame by the display code with the host's current input. */
void input_set_host(const InputState *s);
/* Current state as the game should see it (host input, or the RB_INPUT script for this frame). */
void input_get(InputState *out);
void input_frame(uint64_t frame);
/* Motion from a gamepad's sensors, in 3DS axes (axis mapping per Azahar): acceleration in g, rotation in deg/s.
 * valid = 0 means no sensor: the console then rests flat and still. */
void input_set_motion(const float accel_g[3], const float gyro_dps[3], int valid);
int input_get_motion(float accel_g[3], float gyro_dps[3]);
