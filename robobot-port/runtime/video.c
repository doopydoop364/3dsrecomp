/* Host display: shows the two 3DS screens in one SDL window (top 400x240 above bottom 320x240).
 * RB_HEADLESS=1 runs without a window. RB_SHOT=120,600 writes build/shot_<frame>.png for those frames. */
#include "host.h"
#include <SDL3/SDL.h>
#include <zlib.h>
#include "input.h"
#include "settings.h"

#define OUT_W 400
#define OUT_H 480
static SDL_Window *win;
static SDL_Renderer *ren;
static uint8_t frame_rgba[OUT_W * OUT_H * 4];
static int headless;
static uint64_t shots[16]; static int nshots;
static SDL_Gamepad *pad;
static SDL_Texture *tex_top, *tex_bot;
enum { L_STACKED, L_LARGE_TOP, L_TOP_ONLY, L_SIDE, L_COUNT };
static int layout, swapped;
static SDL_FRect r_top, r_bot;                         /* where each screen is drawn, in window pixels */

/* Controls from settings.toml [controls]; defaults follow the usual 3DS emulator layout. */
static const struct { const char *name, *def; uint32_t bit; } binds_def[] = {
    { "a", "X", BTN_A }, { "b", "Z", BTN_B }, { "x", "S", BTN_X }, { "y", "A", BTN_Y }, { "l", "Q", BTN_L }, { "r", "W", BTN_R },
    { "start", "Return", BTN_START }, { "select", "Backspace", BTN_SELECT },
    { "dpad_up", "I", BTN_UP }, { "dpad_down", "K", BTN_DOWN }, { "dpad_left", "J", BTN_LEFT }, { "dpad_right", "L", BTN_RIGHT },
    { "circle_up", "Up", 0 }, { "circle_down", "Down", 0 }, { "circle_left", "Left", 0 }, { "circle_right", "Right", 0 } };
#define NBINDS (sizeof binds_def / sizeof *binds_def)
static SDL_Scancode binds[NBINDS];
static void load_binds(void) {
    for (unsigned i = 0; i < NBINDS; i++) {
        char key[64]; snprintf(key, sizeof key, "controls.%s", binds_def[i].name);
        const char *name = settings_get(key, binds_def[i].def);
        binds[i] = SDL_GetScancodeFromName(name);
        if (binds[i] == SDL_SCANCODE_UNKNOWN) { fprintf(stderr, "settings: unknown key name '%s' for %s, using %s\n", name, key, binds_def[i].def); binds[i] = SDL_GetScancodeFromName(binds_def[i].def); }
    }
}

static void poll_host_input(void) {
    InputState s; memset(&s, 0, sizeof s);
    const bool *k = SDL_GetKeyboardState(0);
    for (unsigned i = 0; i < 12; i++) if (k[binds[i]]) s.buttons |= binds_def[i].bit;
    int cx = 0, cy = 0;
    if (k[binds[12]]) cy += 154;
    if (k[binds[13]]) cy -= 154;
    if (k[binds[14]]) cx -= 154;
    if (k[binds[15]]) cx += 154;
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
    if (r_bot.w > 0 && (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK)) {   /* stylus: left mouse on the bottom screen */
        float bx = (mx - r_bot.x) * 320.0f / r_bot.w, by = (my - r_bot.y) * 240.0f / r_bot.h;
        if (bx >= 0 && bx < 320 && by >= 0 && by < 240) { s.touch = 1; s.tx = (uint16_t)bx; s.ty = (uint16_t)by; }
    }
    input_set_host(&s);
}

/* Place the screens for the current layout in a window of size w x h, keeping 3DS proportions. */
static SDL_FRect fit(float x, float y, float w, float h, float sw, float sh) {
    float sc = w / sw < h / sh ? w / sw : h / sh;
    SDL_FRect r = { x + (w - sw * sc) / 2, y + (h - sh * sc) / 2, sw * sc, sh * sc };
    return r;
}
static void place_screens(int w, int h, int show_bottom) {
    float W = (float)w, H = (float)h;
    SDL_FRect none = { 0, 0, 0, 0 };
    switch (layout) {
    case L_STACKED: { SDL_FRect all = fit(0, 0, W, H, 400, 480);
        float sc = all.w / 400; r_top = (SDL_FRect){ all.x, all.y, 400 * sc, 240 * sc };
        r_bot = (SDL_FRect){ all.x + 40 * sc, all.y + 240 * sc, 320 * sc, 240 * sc }; break; }
    case L_SIDE: { SDL_FRect all = fit(0, 0, W, H, 720, 240);
        float sc = all.w / 720; r_top = (SDL_FRect){ all.x, all.y, 400 * sc, 240 * sc };
        r_bot = (SDL_FRect){ all.x + 400 * sc, all.y, 320 * sc, 240 * sc }; break; }
    case L_LARGE_TOP: { r_top = fit(0, 0, W, H, 400, 240);
        float bw = r_top.w * 0.3f, bh = bw * 0.75f;      /* bottom screen as a corner overlay */
        r_bot = (SDL_FRect){ r_top.x + r_top.w - bw, r_top.y + r_top.h - bh, bw, bh }; break; }
    default: r_top = fit(0, 0, W, H, 400, 240); r_bot = show_bottom ? fit(W * 0.2f, H * 0.2f, W * 0.6f, H * 0.6f, 320, 240) : none;
    }
    if (swapped && layout != L_TOP_ONLY) {              /* swap which screen gets which place (sizes follow the place) */
        SDL_FRect t = r_top; r_top = r_bot; r_bot = t;
    }
}

void video_init(const char *title) {
    const char *s = getenv("RB_SHOT");
    while (s && *s && nshots < 16) { shots[nshots++] = strtoull(s, (char **)&s, 10); if (*s == ',') s++; }
    headless = getenv("RB_HEADLESS") != 0;
    if (headless) return;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL video is not available (%s), running without a window\n", SDL_GetError()); headless = 1; return; }
    win = SDL_CreateWindow(title, OUT_W * 2, OUT_H * 2, SDL_WINDOW_RESIZABLE);
    ren = win ? SDL_CreateRenderer(win, 0) : 0;
    tex_top = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 400, 240) : 0;
    tex_bot = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 320, 240) : 0;
    if (!tex_top || !tex_bot) fatal("cannot create the window: %s", SDL_GetError());
    SDL_SetTextureScaleMode(tex_top, SDL_SCALEMODE_NEAREST); SDL_SetTextureScaleMode(tex_bot, SDL_SCALEMODE_NEAREST);
    load_binds();
    const char *lname = settings_get("video.layout", "stacked");
    static const char *lnames[L_COUNT] = { "stacked", "large_top", "top_only", "side_by_side" };
    for (int i = 0; i < L_COUNT; i++) if (!strcmp(lname, lnames[i])) layout = i;
    if (settings_int("video.fullscreen", 0)) SDL_SetWindowFullscreen(win, true);
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
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_F1) layout = (layout + 1) % L_COUNT;
            if (e.key.key == SDLK_F2) swapped = !swapped;
            if (e.key.key == SDLK_F11) SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
            if (e.key.key == SDLK_F12) { char p[64]; snprintf(p, sizeof p, "screenshot_%06llu.png", (unsigned long long)frame); write_png(p); }
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) { pad = SDL_OpenGamepad(e.gdevice.which); if (pad) fprintf(stderr, "gamepad: %s\n", SDL_GetGamepadName(pad)); }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) { SDL_CloseGamepad(pad); pad = 0; }
    }
    int w, h;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    place_screens(w, h, SDL_GetKeyboardState(0)[SDL_SCANCODE_F3]);
    poll_host_input();
    SDL_UpdateTexture(tex_top, 0, frame_rgba, OUT_W * 4);
    SDL_UpdateTexture(tex_bot, 0, frame_rgba + ((size_t)240 * OUT_W + 40) * 4, OUT_W * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (r_top.w > 0) SDL_RenderTexture(ren, tex_top, 0, &r_top);
    if (r_bot.w > 0) SDL_RenderTexture(ren, tex_bot, 0, &r_bot);
    SDL_RenderPresent(ren);
}
