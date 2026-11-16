/* Host display: shows the two 3DS screens in one SDL window (top 400x240 above bottom 320x240).
 * RB_HEADLESS=1 runs without a window. RB_SHOT=120,600 writes build/shot_<frame>.png for those frames. */
#include "host.h"
#include <SDL3/SDL.h>
#include <zlib.h>
#include "input.h"

#define OUT_W 400
#define OUT_H 480
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint8_t frame_rgba[OUT_W * OUT_H * 4];
static int headless;
static uint64_t shots[16]; static int nshots;
static SDL_Gamepad *pad;

/* Keyboard defaults: WASD circle pad, arrows D-pad, K/J/I/U = A/B/X/Y, Q/E = L/R, Enter Start, Backspace Select. */
static void poll_host_input(void) {
    InputState s; memset(&s, 0, sizeof s);
    const bool *k = SDL_GetKeyboardState(0);
    static const struct { SDL_Scancode key; uint32_t bit; } keys[] = {
        { SDL_SCANCODE_K, BTN_A }, { SDL_SCANCODE_J, BTN_B }, { SDL_SCANCODE_I, BTN_X }, { SDL_SCANCODE_U, BTN_Y },
        { SDL_SCANCODE_Q, BTN_L }, { SDL_SCANCODE_E, BTN_R }, { SDL_SCANCODE_RETURN, BTN_START }, { SDL_SCANCODE_BACKSPACE, BTN_SELECT },
        { SDL_SCANCODE_UP, BTN_UP }, { SDL_SCANCODE_DOWN, BTN_DOWN }, { SDL_SCANCODE_LEFT, BTN_LEFT }, { SDL_SCANCODE_RIGHT, BTN_RIGHT } };
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) if (k[keys[i].key]) s.buttons |= keys[i].bit;
    int cx = 0, cy = 0;
    if (k[SDL_SCANCODE_A]) cx -= 154;
    if (k[SDL_SCANCODE_D]) cx += 154;
    if (k[SDL_SCANCODE_W]) cy += 154;
    if (k[SDL_SCANCODE_S]) cy -= 154;
    if (pad) {                                         /* positional (Nintendo) layout: east = A, south = B */
        static const struct { SDL_GamepadButton b; uint32_t bit; } pb[] = {
            { SDL_GAMEPAD_BUTTON_EAST, BTN_A }, { SDL_GAMEPAD_BUTTON_SOUTH, BTN_B }, { SDL_GAMEPAD_BUTTON_NORTH, BTN_X },
            { SDL_GAMEPAD_BUTTON_WEST, BTN_Y }, { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R },
            { SDL_GAMEPAD_BUTTON_START, BTN_START }, { SDL_GAMEPAD_BUTTON_BACK, BTN_SELECT }, { SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT } };
        for (unsigned i = 0; i < sizeof pb / sizeof *pb; i++) if (SDL_GetGamepadButton(pad, pb[i].b)) s.buttons |= pb[i].bit;
        int ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax * ax + ay * ay > 6000 * 6000) { cx = ax * 154 / 32767; cy = -ay * 154 / 32767; }
    }
    s.cx = (int16_t)(cx < -154 ? -154 : cx > 154 ? 154 : cx); s.cy = (int16_t)(cy < -154 ? -154 : cy > 154 ? 154 : cy);
    float mx, my;
    if (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) {   /* stylus: left mouse button on the bottom screen */
        float lx, ly;
        SDL_RenderCoordinatesFromWindow(ren, mx, my, &lx, &ly);
        int bx = (int)lx - 40, by = (int)ly - 240;
        if (bx >= 0 && bx < 320 && by >= 0 && by < 240) { s.touch = 1; s.tx = (uint16_t)bx; s.ty = (uint16_t)by; }
    }
    input_set_host(&s);
}

void video_init(const char *title) {
    const char *s = getenv("RB_SHOT");
    while (s && *s && nshots < 16) { shots[nshots++] = strtoull(s, (char **)&s, 10); if (*s == ',') s++; }
    headless = getenv("RB_HEADLESS") != 0;
    if (headless) return;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL video is not available (%s), running without a window\n", SDL_GetError()); headless = 1; return; }
    win = SDL_CreateWindow(title, OUT_W * 2, OUT_H * 2, SDL_WINDOW_RESIZABLE);
    ren = win ? SDL_CreateRenderer(win, 0) : 0;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, OUT_W, OUT_H) : 0;
    if (!tex) fatal("cannot create the window: %s", SDL_GetError());
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderLogicalPresentation(ren, OUT_W, OUT_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

/* LCD framebuffers are stored rotated: 240 pixels per memory row, one memory row per screen column. */
static void blit_screen(const uint8_t *fb, uint32_t stride, uint32_t fmt, int width, int ox, int oy) {
    static const int bpp_of[8] = { 4, 3, 2, 2, 2, 4, 4, 4 };
    int bpp = bpp_of[fmt & 7];
    for (int x = 0; x < width; x++) {
        const uint8_t *col = fb + (size_t)x * stride;
        for (int y = 0; y < 240; y++) {
            const uint8_t *p = col + (239 - y) * bpp;
            uint8_t r, g, b; uint16_t v = (uint16_t)(p[0] | p[1] << 8);
            switch (fmt & 7) {
            case 0: r = p[3]; g = p[2]; b = p[1]; break;
            case 1: r = p[2]; g = p[1]; b = p[0]; break;
            case 2: r = (uint8_t)((v >> 11) << 3 | v >> 13); g = (uint8_t)(((v >> 5) & 63) << 2 | ((v >> 5) & 63) >> 4); b = (uint8_t)((v & 31) << 3 | (v & 31) >> 2); break;
            case 3: r = (uint8_t)((v >> 11) << 3 | v >> 13); g = (uint8_t)(((v >> 6) & 31) << 3 | ((v >> 6) & 31) >> 2); b = (uint8_t)(((v >> 1) & 31) << 3 | ((v >> 1) & 31) >> 2); break;
            default: r = (uint8_t)((v >> 12) * 17); g = (uint8_t)(((v >> 8) & 15) * 17); b = (uint8_t)(((v >> 4) & 15) * 17); break;
            }
            uint8_t *o = frame_rgba + ((size_t)(oy + y) * OUT_W + (size_t)(ox + x)) * 4;
            o[0] = r; o[1] = g; o[2] = b; o[3] = 255;
        }
    }
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t len[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    uint32_t c = (uint32_t)crc32(crc32(0, (const uint8_t *)type, 4), data, n);
    uint8_t crc[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    fwrite(len, 1, 4, f); fwrite(type, 1, 4, f); if (n) fwrite(data, 1, n, f); fwrite(crc, 1, 4, f);
}
static void write_png(const char *path) {
    static uint8_t raw[OUT_H * (OUT_W * 3 + 1)], z[sizeof raw + 4096];
    for (int y = 0; y < OUT_H; y++) {
        uint8_t *row = raw + (size_t)y * (OUT_W * 3 + 1); *row++ = 0;
        for (int x = 0; x < OUT_W; x++) memcpy(row + x * 3, frame_rgba + ((size_t)y * OUT_W + x) * 4, 3);
    }
    uLongf zn = sizeof z;
    FILE *f = fopen(path, "wb");
    if (!f || compress(z, &zn, raw, sizeof raw) != Z_OK) { fprintf(stderr, "cannot write %s\n", path); if (f) fclose(f); return; }
    uint8_t hdr[13] = { 0, 0, OUT_W >> 8, OUT_W & 255, 0, 0, OUT_H >> 8, OUT_H & 255, 8, 2, 0, 0, 0 };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", hdr, 13); png_chunk(f, "IDAT", z, (uint32_t)zn); png_chunk(f, "IEND", 0, 0);
    fclose(f);
    fprintf(stderr, "screenshot written to %s\n", path);
}

/* Called once per vertical blank with the framebuffers the game currently displays (0 = none yet). */
void video_frame(uint64_t frame, const uint8_t *top, uint32_t top_stride, uint32_t top_fmt,
                 const uint8_t *bot, uint32_t bot_stride, uint32_t bot_fmt) {
    int want_shot = 0;
    for (int i = 0; i < nshots; i++) if (shots[i] == frame) want_shot = 1;
    if (headless && !want_shot) return;
    memset(frame_rgba, 0, sizeof frame_rgba);
    if (top) blit_screen(top, top_stride, top_fmt, 400, 0, 0);
    if (bot) blit_screen(bot, bot_stride, bot_fmt, 320, 40, 240);
    if (want_shot) { char p[64]; snprintf(p, sizeof p, "build/shot_%06llu.png", (unsigned long long)frame); write_png(p); }
    if (headless) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) { fprintf(stderr, "window closed\n"); exit(0); }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) { pad = SDL_OpenGamepad(e.gdevice.which); if (pad) fprintf(stderr, "gamepad: %s\n", SDL_GetGamepadName(pad)); }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) { SDL_CloseGamepad(pad); pad = 0; }
    }
    poll_host_input();
    SDL_UpdateTexture(tex, 0, frame_rgba, OUT_W * 4);
    SDL_RenderClear(ren); SDL_RenderTexture(ren, tex, 0, 0); SDL_RenderPresent(ren);
}
