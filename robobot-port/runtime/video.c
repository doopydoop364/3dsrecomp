/* Host display: shows the two 3DS screens in one SDL window (top 400x240 above bottom 320x240).
 * RB_HEADLESS=1 runs without a window. RB_SHOT=120,600 writes build/shot_<frame>.png for those frames. */
#include "host.h"
#include <SDL3/SDL.h>
#include <zlib.h>
#include <time.h>
#include "input.h"
#include "settings.h"
#include "vkr.h"
void nfc_host_tap(void); void nfc_host_remove(void);   /* nfc.c */

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
static SDL_Scancode ff_key; static int ff_toggle, ff_on;
static int show_fps;                                   /* F9: game frames/s and window frames/s, top left */
uint64_t gpu_swaps(void);   /* fast-forward: hold (or toggle) a key */

/* Controls from settings.toml [controls]; defaults follow the usual 3DS emulator layout. */
static const struct { const char *name, *def; uint32_t bit; } binds_def[] = {
    { "a", "L", BTN_A }, { "b", "K", BTN_B }, { "x", "I", BTN_X }, { "y", "J", BTN_Y }, { "l", "Q", BTN_L }, { "r", "E", BTN_R },
    { "start", "=", BTN_START }, { "select", "-", BTN_SELECT },
    { "dpad_up", "Up", BTN_UP }, { "dpad_down", "Down", BTN_DOWN }, { "dpad_left", "Left", BTN_LEFT }, { "dpad_right", "Right", BTN_RIGHT },
    { "circle_up", "W", 0 }, { "circle_down", "S", 0 }, { "circle_left", "A", 0 }, { "circle_right", "D", 0 } };
#define NBINDS (sizeof binds_def / sizeof *binds_def)
#define NALT 4                                          /* keys per control: "Left Shift, Q" binds both */
static SDL_Scancode binds[NBINDS][NALT];
static void load_binds(void) {
    for (unsigned i = 0; i < NBINDS; i++) {
        char key[64]; snprintf(key, sizeof key, "controls.%s", binds_def[i].name);
        char list[64]; snprintf(list, sizeof list, "%s", settings_get(key, binds_def[i].def));
        int n = 0;
        for (char *t = strtok(list, ","); t && n < NALT; t = strtok(0, ",")) {
            while (*t == ' ') t++;
            char *e = t + strlen(t); while (e > t && e[-1] == ' ') *--e = 0;
            SDL_Scancode sc = SDL_GetScancodeFromName(t);
            if (sc == SDL_SCANCODE_UNKNOWN) fprintf(stderr, "settings: unknown key name '%s' for %s\n", t, key);
            else binds[i][n++] = sc;
        }
        if (!n) binds[i][n++] = SDL_GetScancodeFromName(binds_def[i].def);
        while (n < NALT) binds[i][n++] = SDL_SCANCODE_UNKNOWN;
    }
}
static int bind_down(const bool *k, unsigned i) {
    for (int j = 0; j < NALT; j++) if (binds[i][j] != SDL_SCANCODE_UNKNOWN && k[binds[i][j]]) return 1;
    return 0;
}

static void poll_host_input(void) {
    InputState s; memset(&s, 0, sizeof s);
    const bool *k = SDL_GetKeyboardState(0);
    for (unsigned i = 0; i < 12; i++) if (bind_down(k, i)) s.buttons |= binds_def[i].bit;
    int cx = 0, cy = 0;
    if (bind_down(k, 12)) cy += 154;
    if (bind_down(k, 13)) cy -= 154;
    if (bind_down(k, 14)) cx -= 154;
    if (bind_down(k, 15)) cx += 154;
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
    ff_key = SDL_GetScancodeFromName(settings_get("controls.fast_forward", "Tab"));
    ff_toggle = !strcmp(settings_get("controls.fast_forward_mode", "hold"), "toggle");
    show_fps = settings_int("video.show_fps", 0);
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
static void write_png_buf(const char *path, const uint8_t *rgba, uint32_t W, uint32_t H) {
    size_t rawn = (size_t)H * (W * 3 + 1);
    uint8_t *raw = malloc(rawn); uLongf zn = compressBound(rawn); uint8_t *z = malloc(zn);
    if (!raw || !z) { free(raw); free(z); return; }
    for (uint32_t y = 0; y < H; y++) {
        uint8_t *row = raw + (size_t)y * (W * 3 + 1); *row++ = 0;
        for (uint32_t x = 0; x < W; x++) memcpy(row + x * 3, rgba + ((size_t)y * W + x) * 4, 3);
    }
    FILE *f = fopen(path, "wb");
    if (!f || compress(z, &zn, raw, rawn) != Z_OK) { fprintf(stderr, "cannot write %s\n", path); if (f) fclose(f); free(raw); free(z); return; }
    uint8_t hdr[13] = { (uint8_t)(W >> 24), (uint8_t)(W >> 16), (uint8_t)(W >> 8), (uint8_t)W, (uint8_t)(H >> 24), (uint8_t)(H >> 16), (uint8_t)(H >> 8), (uint8_t)H, 8, 2, 0, 0, 0 };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    png_chunk(f, "IHDR", hdr, 13); png_chunk(f, "IDAT", z, (uint32_t)zn); png_chunk(f, "IEND", 0, 0);
    fclose(f); free(raw); free(z);
    fprintf(stderr, "screenshot written to %s\n", path);
}
static void write_png(const char *path) { write_png_buf(path, frame_rgba, OUT_W, OUT_H); }

/* Full-resolution frame (render scale S): native frame upscaled, then screens whose image came straight from a
 * scaled Vulkan surface are redrawn from it. Same orientation rules as blit_screen. */
static uint8_t *frame_hi; static uint32_t hi_scale;
static int compose_hires(const uint8_t *top, const uint8_t *bot) {
    const uint8_t *img[2] = { 0, 0 }; uint32_t iw, ih, S = 1, s2; int flip[2] = { 0, 0 };
    if (top) img[0] = vkr_hires((uint32_t)(uintptr_t)top, 400, &iw, &ih, &flip[0], &S);
    if (bot) img[1] = vkr_hires((uint32_t)(uintptr_t)bot, 320, &iw, &ih, &flip[1], &s2);
    if (img[1] && !img[0]) S = s2;
    if (!img[0] && !img[1]) return 0;
    if (S != hi_scale) { free(frame_hi); frame_hi = malloc((size_t)OUT_W * OUT_H * S * S * 4); hi_scale = S; }
    if (!frame_hi) return 0;
    uint32_t W = OUT_W * S;
    memset(frame_hi, 0, (size_t)W * OUT_H * S * 4);
    for (int sc = 0; sc < 2; sc++) {
        uint32_t ws = (sc ? 320 : 400) * S, ox = (sc ? 40 : 0) * S, oy = (sc ? 240 : 0) * S, pw = 240 * S;
        if (!img[sc]) {                                    /* no scaled source: enlarge the native screen */
            for (uint32_t y = 0; y < pw; y++) {
                uint32_t *o = (uint32_t *)(frame_hi + ((size_t)(oy + y) * W + ox) * 4);
                const uint32_t *in = (const uint32_t *)(frame_rgba + ((size_t)((oy + y) / S) * OUT_W + ox / S) * 4);
                for (uint32_t x = 0; x < ws; x++) o[x] = in[x / S];
            }
            continue;
        }
        for (uint32_t X = 0; X < ws; X++) {
            uint32_t row = flip[sc] ? ws - 1 - X : X, py = ws - 1 - row;
            for (uint32_t Y = 0; Y < pw; Y++) {
                const uint8_t *p = img[sc] + ((size_t)py * pw + (pw - 1 - Y)) * 4;
                uint8_t *o = frame_hi + ((size_t)(oy + Y) * W + ox + X) * 4;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255;
            }
        }
    }
    return 1;
}
static SDL_Texture *tex_top_hi, *tex_bot_hi; static uint32_t tex_hi_scale;

/* Called once per vertical blank with the framebuffers the game currently displays (0 = none yet). */
void video_frame(uint64_t frame, const uint8_t *top, uint32_t top_stride, uint32_t top_fmt,
                 const uint8_t *bot, uint32_t bot_stride, uint32_t bot_fmt) {
    int want_shot = 0;
    for (int i = 0; i < nshots; i++) if (shots[i] == frame) want_shot = 1;
    static int prof = -1; if (prof < 0) prof = getenv("RB_PROFILE_PRESENT") != 0;   /* test: time the window path headless */
    if (headless && !want_shot && !prof) return;
    memset(frame_rgba, 0, sizeof frame_rgba);
    if (top) blit_screen(top, top_stride, top_fmt, 400, 0, 0);
    if (bot) blit_screen(bot, bot_stride, bot_fmt, 320, 40, 240);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int hi = compose_hires(top, bot);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (prof) { static double acc; static int n; acc += (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
                if (++n == 300) { fprintf(stderr, "present: compose %.2f ms/frame\n", acc / n); acc = 0; n = 0; } }
    if (want_shot) {
        char p[64]; snprintf(p, sizeof p, "build/shot_%06llu.png", (unsigned long long)frame); write_png(p);
        if (hi) { snprintf(p, sizeof p, "build/shot_%06llu_hi.png", (unsigned long long)frame); write_png_buf(p, frame_hi, OUT_W * hi_scale, OUT_H * hi_scale); }
    }
    if (headless) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) { fprintf(stderr, "window closed\n"); exit(0); }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_F1) layout = (layout + 1) % L_COUNT;
            if (e.key.key == SDLK_F2) swapped = !swapped;
            if (ff_toggle && e.key.scancode == ff_key) { ff_on = !ff_on; k_set_fast_forward(ff_on); }
            if (e.key.key == SDLK_F9) show_fps = !show_fps;
            if (e.key.key == SDLK_F5) nfc_host_tap();             /* amiibo: tap the next dump from <game>/amiibo */
            if (e.key.key == SDLK_F6) nfc_host_remove();
            if (e.key.key == SDLK_F11) SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
            if (e.key.key == SDLK_F12) { char p[64]; snprintf(p, sizeof p, "screenshot_%06llu.png", (unsigned long long)frame);
                                         if (hi) write_png_buf(p, frame_hi, OUT_W * hi_scale, OUT_H * hi_scale); else write_png(p); }
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) { pad = SDL_OpenGamepad(e.gdevice.which); if (pad) fprintf(stderr, "gamepad: %s\n", SDL_GetGamepadName(pad)); }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) { SDL_CloseGamepad(pad); pad = 0; }
    }
    int w, h;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    place_screens(w, h, SDL_GetKeyboardState(0)[SDL_SCANCODE_F3]);
    poll_host_input();
    if (!ff_toggle && ff_key != SDL_SCANCODE_UNKNOWN) { int on = SDL_GetKeyboardState(0)[ff_key]; if (on != ff_on) { ff_on = on; k_set_fast_forward(on); } }
    SDL_Texture *tt = tex_top, *tb = tex_bot;
    if (hi) {
        uint32_t S = hi_scale;
        if (tex_hi_scale != S) {
            if (tex_top_hi) SDL_DestroyTexture(tex_top_hi);
            if (tex_bot_hi) SDL_DestroyTexture(tex_bot_hi);
            tex_top_hi = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)(400 * S), (int)(240 * S));
            tex_bot_hi = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)(320 * S), (int)(240 * S));
            SDL_SetTextureScaleMode(tex_top_hi, SDL_SCALEMODE_LINEAR); SDL_SetTextureScaleMode(tex_bot_hi, SDL_SCALEMODE_LINEAR);
            tex_hi_scale = S;
        }
        SDL_UpdateTexture(tex_top_hi, 0, frame_hi, (int)(OUT_W * S * 4));
        SDL_UpdateTexture(tex_bot_hi, 0, frame_hi + ((size_t)240 * S * OUT_W * S + 40 * S) * 4, (int)(OUT_W * S * 4));
        tt = tex_top_hi; tb = tex_bot_hi;
    } else {
        SDL_UpdateTexture(tex_top, 0, frame_rgba, OUT_W * 4);
        SDL_UpdateTexture(tex_bot, 0, frame_rgba + ((size_t)240 * OUT_W + 40) * 4, OUT_W * 4);
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (r_top.w > 0) SDL_RenderTexture(ren, tt, 0, &r_top);
    if (r_bot.w > 0) SDL_RenderTexture(ren, tb, 0, &r_bot);
    if (show_fps) {                                    /* counts over the last half second of wall time */
        static uint64_t t_last, sw_last, presents; static float game_fps, win_fps;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        presents++;
        if (!t_last) { t_last = now; sw_last = gpu_swaps(); presents = 0; }
        else if (now - t_last >= 500000000ull) {
            double dt = (double)(now - t_last) / 1e9;
            game_fps = (float)((double)(gpu_swaps() - sw_last) / dt); win_fps = (float)((double)presents / dt);
            t_last = now; sw_last = gpu_swaps(); presents = 0;
        }
        char txt[64]; snprintf(txt, sizeof txt, "game %.0f fps  window %.0f fps%s", game_fps, win_fps, ff_on ? "  >>" : "");
        SDL_SetRenderScale(ren, 2.0f, 2.0f);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 160); SDL_FRect bg = { 2, 2, (float)strlen(txt) * 8 + 4, 12 }; SDL_RenderFillRect(ren, &bg);
        SDL_SetRenderDrawColor(ren, 255, 255, 80, 255); SDL_RenderDebugText(ren, 4, 4, txt);
        SDL_SetRenderScale(ren, 1.0f, 1.0f);
    }
    SDL_RenderPresent(ren);
}
