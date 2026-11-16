/* Host display: shows the two 3DS screens in one SDL window (top 400x240 above bottom 320x240).
 * RB_HEADLESS=1 runs without a window. RB_SHOT=120,600 writes build/shot_<frame>.png for those frames. */
#include "host.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <zlib.h>
#include <time.h>
#include "input.h"
#include "settings.h"
#include "vkr.h"
#include "audio.h"
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
static int show_fps;
/* In-game profiler: F10 records the next 300 frames and writes profile_<frame>.txt (where the time per frame went). */
void gpu_counters(uint64_t out[3]); void pica_counters(uint64_t out[9]);
static int prof_left; static uint64_t prof_frame0, prof_wall0, prof_p0[9], prof_v0[4], prof_g0[3], prof_paced0, prof_sw0[8], prof_dl0[8]; static double prof_emu_max;
extern uint64_t k_paced_ns;
static uint64_t wall_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void prof_start(uint64_t frame) {
    prof_left = 300; prof_frame0 = frame; prof_wall0 = wall_now(); pica_counters(prof_p0); vkr_counters(prof_v0); gpu_counters(prof_g0);
    prof_paced0 = k_paced_ns; prof_emu_max = 0; vkr_sync_counts(prof_sw0, prof_dl0);
    fprintf(stderr, "profiler: recording 300 frames\n");
}
static void prof_end(uint64_t frame) {
    uint64_t p[9], v[4], g[3]; pica_counters(p); vkr_counters(v); gpu_counters(g);
    double n = (double)(frame - prof_frame0), wall = (double)(wall_now() - prof_wall0) / 1e6 / n, slept = (double)(k_paced_ns - prof_paced0) / 1e6 / n;
#define D(a, b) ((double)((a) - (b)) / 1e6 / n)
    double lists = D(g[0], prof_g0[0]), lists_vk = D(g[2], prof_g0[2]), transfers = D(g[1], prof_g0[1]);
    double vkwait = D(v[0], prof_v0[0]) + D(v[1], prof_v0[1]), vk_outside = vkwait - lists_vk;
    double other = wall - slept - lists - transfers - vk_outside;
    char path[64]; snprintf(path, sizeof path, "profile_%06llu.txt", (unsigned long long)frame);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "frames %llu-%llu: %.2f ms per frame (%.1f fps); %.2f ms of that was pacing sleep; worst emulation frame %.1f ms\n",
                (unsigned long long)prof_frame0, (unsigned long long)frame, wall, 1000.0 / wall, slept, prof_emu_max);
        fprintf(f, "GPU command lists %.2f ms per frame, of which:\n", lists);
        fprintf(f, "  vertex shading %.2f (main thread waiting for shading threads %.2f), clipping+assembly %.2f, draw setup %.2f,\n"
                   "  Vulkan recording %.2f, Vulkan waits/write-backs inside lists %.2f\n",
                D(p[1], prof_p0[1]), D(p[7], prof_p0[7]), D(p[2], prof_p0[2]), D(p[0], prof_p0[0]), D(p[3], prof_p0[3]), lists_vk);
        fprintf(f, "Vulkan waits/write-backs outside lists (display, vblank) %.2f ms; display transfers %.2f ms; textures %.2f ms (%.1f uploads)\n",
                vk_outside, transfers, D(v[2], prof_v0[2]), (double)(v[3] - prof_v0[3]) / n);
        fprintf(f, "everything else (game code, services, audio, window) %.2f ms\n", other);
        { uint64_t sw[8], dl[8]; vkr_sync_counts(sw, dl);
          static const char *why[8] = { "other", "DMA", "memory fill", "display transfer", "texture copy", "vblank", "new surface", "texture read" };
          fprintf(f, "GPU sync waits per frame by cause (waits / surface write-backs):");
          for (int i = 0; i < 8; i++) if (sw[i] != prof_sw0[i] || dl[i] != prof_dl0[i])
              fprintf(f, " %s %.2f/%.2f;", why[i], (double)(sw[i] - prof_sw0[i]) / n, (double)(dl[i] - prof_dl0[i]) / n);
          fprintf(f, "\n"); }
        fprintf(f, "per frame: %.0f draws, %.0f triangles drawn (%.0f submitted triangles needed no clipping), %.0f vertices shaded\n",
                (double)(p[4] - prof_p0[4]) / n, (double)(p[5] - prof_p0[5]) / n, (double)(p[8] - prof_p0[8]) / n, (double)(p[6] - prof_p0[6]) / n);
        fclose(f);
    }
#undef D
    fprintf(stderr, "profiler: written to %s\n", path);
}
/* video.present = "vulkan": Vulkan draws the window (vk.c); SDL's software renderer draws only the overlay (menu,
 * FPS) into ovl_surf, which Vulkan composites on top. Falls back to the SDL renderer if anything fails. */
static int vkpresent, fxaa_on; static SDL_Surface *ovl_surf; static VkrScreen cur_scr[2]; static char ovl_last[128] = "\1";
static int make_surface(void *instance, uint64_t *out) {
    VkSurfaceKHR sf;
    if (!SDL_Vulkan_CreateSurface(win, (VkInstance)instance, NULL, &sf)) { fprintf(stderr, "vulkan: no window surface (%s)\n", SDL_GetError()); return 0; }
    *out = (uint64_t)(uintptr_t)sf; return 1;
}
static int make_headless_surface(void *instance, uint64_t *out) {
    PFN_vkCreateHeadlessSurfaceEXT f = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr((VkInstance)instance, "vkCreateHeadlessSurfaceEXT");
    VkHeadlessSurfaceCreateInfoEXT ci = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR sf;
    if (!f || f((VkInstance)instance, &ci, 0, &sf) != VK_SUCCESS) return 0;
    *out = (uint64_t)(uintptr_t)sf; return 1;
}
static void ovl_fit(int w, int h) {                     /* overlay surface the size of the window */
    if (ovl_surf && ovl_surf->w == w && ovl_surf->h == h) return;
    if (ren) SDL_DestroyRenderer(ren);
    if (ovl_surf) SDL_DestroySurface(ovl_surf);
    ovl_surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    ren = ovl_surf ? SDL_CreateSoftwareRenderer(ovl_surf) : 0;
    if (!ren) fatal("cannot create the overlay: %s", SDL_GetError());
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    ovl_last[0] = 1;                                     /* redraw */
}
static double present_ms, emu_ms, frame_end;
extern uint64_t k_paced_ns; static uint64_t paced_last;      /* FPS overlay timing: smoothed ms per frame */
static void menu_init(void);
/* Touch hotspots from [hotspots] (per-game file or settings.toml): holding the key touches that point. */
static struct { SDL_Scancode key; uint16_t x, y; } hot[32]; static int nhot;
static void add_hotspot(const char *name, const char *value) {
    int x, y; SDL_Scancode k = SDL_GetScancodeFromName(name);
    if (nhot == 32 || k == SDL_SCANCODE_UNKNOWN || sscanf(value, "%d , %d", &x, &y) != 2 || x < 0 || x > 319 || y < 0 || y > 239)
        { fprintf(stderr, "settings: hotspot %s = \"%s\" ignored (want a key name and \"x,y\" on the 320x240 touch screen)\n", name, value); return; }
    hot[nhot].key = k; hot[nhot].x = (uint16_t)x; hot[nhot].y = (uint16_t)y; nhot++;
}                                   /* F9: game frames/s and window frames/s, top left */
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
        float ac[3], gy[3];
        if (SDL_GamepadSensorEnabled(pad, SDL_SENSOR_ACCEL) && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, ac, 3) &&
            SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gy, 3)) {          /* SDL: m/s^2 and rad/s; axes as Azahar maps them */
            float a[3] = { ac[0] / SDL_STANDARD_GRAVITY, -ac[1] / SDL_STANDARD_GRAVITY, ac[2] / SDL_STANDARD_GRAVITY };
            float g[3] = { -gy[0] * 57.29578f, gy[1] * 57.29578f, -gy[2] * 57.29578f };
            input_set_motion(a, g, 1);
        }
        if (ax * ax + ay * ay > 6000 * 6000) { cx = ax * 154 / 32767; cy = -ay * 154 / 32767; }
    }
    s.cx = (int16_t)(cx < -154 ? -154 : cx > 154 ? 154 : cx); s.cy = (int16_t)(cy < -154 ? -154 : cy > 154 ? 154 : cy);
    float mx, my;
    if (r_bot.w > 0 && (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK)) {   /* stylus: left mouse on the bottom screen */
        float bx = (mx - r_bot.x) * 320.0f / r_bot.w, by = (my - r_bot.y) * 240.0f / r_bot.h;
        if (bx >= 0 && bx < 320 && by >= 0 && by < 240) { s.touch = 1; s.tx = (uint16_t)bx; s.ty = (uint16_t)by; }
    }
    for (int i = 0; i < nhot; i++) if (k[hot[i].key]) { s.touch = 1; s.tx = hot[i].x; s.ty = hot[i].y; }
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
    if (headless && getenv("RB_VK_HEADLESS_PRESENT")) {    /* test: Vulkan presentation to a headless surface */
        static const char *ex[2] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
        vkpresent = vkr_init_present(ex, 2, make_headless_surface);
        fxaa_on = !strcmp(settings_get("video.antialias", "off"), "fxaa");
        fprintf(stderr, "video: headless Vulkan presentation %s\n", vkpresent ? "on" : "unavailable");
    }
    if (headless) return;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL video is not available (%s), running without a window\n", SDL_GetError()); headless = 1; return; }
    const char *rend = getenv("RB_RENDERER");
    int want_vk = !strcmp(settings_get("video.present", "vulkan"), "vulkan") && (!rend || !strcmp(rend, "vulkan"));
    fxaa_on = !strcmp(settings_get("video.antialias", "off"), "fxaa");
    if (want_vk) {
        win = SDL_CreateWindow(title, OUT_W * 2, OUT_H * 2, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        Uint32 n = 0; const char *const *ex = win ? SDL_Vulkan_GetInstanceExtensions(&n) : 0;
        if (ex && vkr_init_present(ex, n, make_surface)) vkpresent = 1;
        else { fprintf(stderr, "video: Vulkan presentation is not available, using the SDL renderer\n"); if (win) SDL_DestroyWindow(win); win = 0; }
    }
    if (vkpresent) {
        int w, h; SDL_GetWindowSizeInPixels(win, &w, &h); ovl_fit(w, h);
        tex_top = tex_bot = 0;
        goto window_ready;
    }
    win = SDL_CreateWindow(title, OUT_W * 2, OUT_H * 2, SDL_WINDOW_RESIZABLE);
    ren = win ? SDL_CreateRenderer(win, 0) : 0;
    if (ren) SDL_SetRenderVSync(ren, settings_int("video.vsync", 0) ? 1 : 0);   /* never block the emulation on the monitor
                                                                                (it may not run at 60 Hz) */
    tex_top = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 400, 240) : 0;
    tex_bot = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 320, 240) : 0;
    if (!tex_top || !tex_bot) fatal("cannot create the window: %s", SDL_GetError());
    SDL_SetTextureScaleMode(tex_top, SDL_SCALEMODE_NEAREST); SDL_SetTextureScaleMode(tex_bot, SDL_SCALEMODE_NEAREST);
window_ready:
    load_binds();
    ff_key = SDL_GetScancodeFromName(settings_get("controls.fast_forward", "Tab"));
    ff_toggle = !strcmp(settings_get("controls.fast_forward_mode", "hold"), "toggle");
    show_fps = settings_int("video.show_fps", 0);
    settings_each("hotspots", add_hotspot);
    menu_init();
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

/* FPS counter text: counts over the last half second of wall time (the numbers change twice a second). */
static void fps_text(char *txt, size_t n) {
    static uint64_t t_last, sw_last, presents; static float game_fps, win_fps; static double e_ms, p_ms;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    presents++;
    if (!t_last) { t_last = now; sw_last = gpu_swaps(); presents = 0; }
    else if (now - t_last >= 500000000ull) {
        double dt = (double)(now - t_last) / 1e9;
        game_fps = (float)((double)(gpu_swaps() - sw_last) / dt); win_fps = (float)((double)presents / dt);
        t_last = now; sw_last = gpu_swaps(); presents = 0; e_ms = emu_ms; p_ms = present_ms;
    }
    snprintf(txt, n, "game %.0f fps  window %.0f fps  emu %.1f ms  present %.1f ms%s%s", game_fps, win_fps, e_ms, p_ms, ff_on ? "  >>" : "", prof_left ? "  [profiling]" : "");
}
static void draw_fps(const char *txt) {
    SDL_SetRenderScale(ren, 2.0f, 2.0f);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160); SDL_FRect bg = { 2, 2, (float)strlen(txt) * 8 + 4, 12 }; SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 255, 255, 80, 255); SDL_RenderDebugText(ren, 4, 4, txt);
    SDL_SetRenderScale(ren, 1.0f, 1.0f);
}
/* Vulkan presentation of the current screens, with the overlay surface when it has something on it. */
static void vk_show(int overlay_on, int overlay_dirty) {
    int w, h; SDL_GetWindowSizeInPixels(win, &w, &h);
    cur_scr[0].rect[0] = r_top.x; cur_scr[0].rect[1] = r_top.y; cur_scr[0].rect[2] = r_top.w; cur_scr[0].rect[3] = r_top.h;
    cur_scr[1].rect[0] = r_bot.x; cur_scr[1].rect[1] = r_bot.y; cur_scr[1].rect[2] = r_bot.w; cur_scr[1].rect[3] = r_bot.h;
    vkr_present(cur_scr, (uint32_t)w, (uint32_t)h, overlay_on ? (const uint8_t *)ovl_surf->pixels : 0,
                (uint32_t)(ovl_surf->pitch / 4), (uint32_t)ovl_surf->h, overlay_dirty, fxaa_on);
}

/* ---- overlay menu (Esc): the game is paused while it is open ---- */
static int menu_open;
static const char *lnames_all[L_COUNT] = { "stacked", "large_top", "top_only", "side_by_side" };
static const char *filters[3] = { "console", "nearest", "linear" }, *renderers[2] = { "vulkan", "software" };
static const int anisos[5] = { 1, 2, 4, 8, 16 };
static int m_volume = 100, m_scale = 1, m_filter, m_aniso, m_renderer, m_saved_msg;
static int bind_dirty[NBINDS], ffkey_dirty;
static int idx_of(const char *v, const char **list, int n) { for (int i = 0; i < n; i++) if (!strcmp(v, list[i])) return i; return 0; }
static void menu_init(void) {
    m_volume = settings_int("audio.volume", 100);
    m_scale = settings_int("video.scale", 1); if (m_scale < 1 || m_scale > 4) m_scale = 1;
    m_filter = idx_of(settings_get("video.texture_filter", "console"), filters, 3);
    m_renderer = idx_of(settings_get("video.renderer", "vulkan"), renderers, 2);
    int a = settings_int("video.anisotropy", 1); m_aniso = 0; for (int i = 0; i < 5; i++) if (anisos[i] <= a) m_aniso = i;
}
static void text(float x, float y, float sc, const char *t, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderScale(ren, sc, sc); SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderDebugText(ren, x / sc, y / sc, t); SDL_SetRenderScale(ren, 1, 1);
}
enum { MI_RESUME, MI_LAYOUT, MI_SWAP, MI_FULL, MI_VOLUME, MI_FPS, MI_FFMODE, MI_AA, MI_SCALE, MI_FILTER, MI_ANISO, MI_RENDERER,
       MI_CONTROLS, MI_SAVE, MI_QUIT, MI_N };
static void item_text(int i, char *o, size_t n) {
    int full = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0;
    switch (i) {
    case MI_RESUME: snprintf(o, n, "Resume"); break;
    case MI_LAYOUT: snprintf(o, n, "Layout          < %s >", lnames_all[layout]); break;
    case MI_SWAP: snprintf(o, n, "Swap screens    < %s >", swapped ? "on" : "off"); break;
    case MI_FULL: snprintf(o, n, "Fullscreen      < %s >", full ? "on" : "off"); break;
    case MI_VOLUME: snprintf(o, n, "Volume          < %d%% >", m_volume); break;
    case MI_FPS: snprintf(o, n, "FPS counter     < %s >", show_fps ? "on" : "off"); break;
    case MI_FFMODE: snprintf(o, n, "Fast-forward    < %s >", ff_toggle ? "toggle" : "hold"); break;
    case MI_AA: snprintf(o, n, vkpresent ? "Anti-aliasing   < %s >" : "Anti-aliasing   < %s >  (Vulkan window only)", fxaa_on ? "fxaa" : "off"); break;
    case MI_SCALE: snprintf(o, n, "Resolution      < %dx >  (restart)", m_scale); break;
    case MI_FILTER: snprintf(o, n, "Textures        < %s >  (restart)", filters[m_filter]); break;
    case MI_ANISO: snprintf(o, n, "Anisotropy      < %dx >  (restart)", anisos[m_aniso]); break;
    case MI_RENDERER: snprintf(o, n, "Renderer        < %s >  (restart)", renderers[m_renderer]); break;
    case MI_CONTROLS: snprintf(o, n, "Controls..."); break;
    case MI_SAVE: snprintf(o, n, m_saved_msg ? "Save settings   (saved to settings.toml)" : "Save settings"); break;
    default: snprintf(o, n, "Quit game");
    }
}
static void item_change(int i, int d) {
    switch (i) {
    case MI_LAYOUT: layout = (layout + d + L_COUNT) % L_COUNT; break;
    case MI_SWAP: swapped = !swapped; break;
    case MI_FULL: SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN)); break;
    case MI_VOLUME: m_volume += 10 * d; if (m_volume < 0) m_volume = 0; if (m_volume > 200) m_volume = 200; audio_set_volume(m_volume); break;
    case MI_FPS: show_fps = !show_fps; break;
    case MI_FFMODE: ff_toggle = !ff_toggle; ff_on = 0; k_set_fast_forward(0); break;
    case MI_AA: fxaa_on = !fxaa_on; break;
    case MI_SCALE: m_scale += d; if (m_scale < 1) m_scale = 4; if (m_scale > 4) m_scale = 1; break;
    case MI_FILTER: m_filter = (m_filter + d + 3) % 3; break;
    case MI_ANISO: m_aniso = (m_aniso + d + 5) % 5; break;
    case MI_RENDERER: m_renderer = !m_renderer; break;
    default: return;
    }
    m_saved_msg = 0;
}
static void menu_save(void) {
    char v[32];
    settings_save("video.layout", lnames_all[layout]);
    settings_save("video.fullscreen", (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) ? "true" : "false");
    snprintf(v, sizeof v, "%d", m_volume); settings_save("audio.volume", v);
    settings_save("video.show_fps", show_fps ? "true" : "false");
    settings_save("video.antialias", fxaa_on ? "fxaa" : "off");
    settings_save("controls.fast_forward_mode", ff_toggle ? "toggle" : "hold");
    snprintf(v, sizeof v, "%d", m_scale); settings_save("video.scale", v);
    settings_save("video.texture_filter", filters[m_filter]);
    snprintf(v, sizeof v, "%d", anisos[m_aniso]); settings_save("video.anisotropy", v);
    settings_save("video.renderer", renderers[m_renderer]);
    for (unsigned i = 0; i < NBINDS; i++) if (bind_dirty[i]) {
        char k[64]; snprintf(k, sizeof k, "controls.%s", binds_def[i].name);
        settings_save(k, SDL_GetScancodeName(binds[i][0]));
    }
    if (ffkey_dirty) settings_save("controls.fast_forward", SDL_GetScancodeName(ff_key));
    m_saved_msg = 1;
}
static void menu_loop(SDL_Texture *tt, SDL_Texture *tb) {
    int sel = 0, page = 0, csel = 0, waiting = 0;
    int nctl = (int)NBINDS + 1;                          /* the controls page: every 3DS control, then fast-forward */
    while (menu_open) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { fprintf(stderr, "window closed\n"); exit(0); }
            int up = 0, down = 0, left = 0, right = 0, ok = 0, back = 0;
            if (e.type == SDL_EVENT_KEY_DOWN) {
                if (waiting) {                               /* rebinding: the next key pressed becomes the binding */
                    if (e.key.scancode != SDL_SCANCODE_ESCAPE) {
                        if (csel < (int)NBINDS) { binds[csel][0] = e.key.scancode; for (int j = 1; j < NALT; j++) binds[csel][j] = SDL_SCANCODE_UNKNOWN; bind_dirty[csel] = 1; }
                        else { ff_key = e.key.scancode; ffkey_dirty = 1; }
                        m_saved_msg = 0;
                    }
                    waiting = 0; continue;
                }
                SDL_Scancode c = e.key.scancode;
                up = c == SDL_SCANCODE_UP || c == SDL_SCANCODE_W; down = c == SDL_SCANCODE_DOWN || c == SDL_SCANCODE_S;
                left = c == SDL_SCANCODE_LEFT || c == SDL_SCANCODE_A; right = c == SDL_SCANCODE_RIGHT || c == SDL_SCANCODE_D;
                ok = c == SDL_SCANCODE_RETURN || c == SDL_SCANCODE_SPACE || c == SDL_SCANCODE_KP_ENTER; back = c == SDL_SCANCODE_ESCAPE;
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                int b = e.gbutton.button;
                up = b == SDL_GAMEPAD_BUTTON_DPAD_UP; down = b == SDL_GAMEPAD_BUTTON_DPAD_DOWN; left = b == SDL_GAMEPAD_BUTTON_DPAD_LEFT;
                right = b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT; ok = b == SDL_GAMEPAD_BUTTON_EAST;          /* Nintendo positions: east = A, south = B */
                back = b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_GUIDE || b == SDL_GAMEPAD_BUTTON_START;
            } else continue;
            if (page == 0) {
                if (up) sel = (sel + MI_N - 1) % MI_N;
                if (down) sel = (sel + 1) % MI_N;
                if (left) item_change(sel, -1);
                if (right) item_change(sel, 1);
                if (ok) {
                    if (sel == MI_RESUME) menu_open = 0;
                    else if (sel == MI_CONTROLS) { page = 1; csel = 0; }
                    else if (sel == MI_SAVE) menu_save();
                    else if (sel == MI_QUIT) { fprintf(stderr, "quit from the menu\n"); exit(0); }
                    else item_change(sel, 1);
                }
                if (back) menu_open = 0;
            } else {
                if (up) csel = (csel + nctl - 1) % nctl;
                if (down) csel = (csel + 1) % nctl;
                if (ok) waiting = 1;
                if (back) page = 0;
            }
        }
        int w, h; SDL_GetWindowSizeInPixels(win, &w, &h);
        place_screens(w, h, 0);
        if (vkpresent) { ovl_fit(w, h); SDL_SetRenderDrawColor(ren, 0, 0, 0, 0); SDL_RenderClear(ren); }
        else {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
            if (r_top.w > 0) SDL_RenderTexture(ren, tt, 0, &r_top);
            if (r_bot.w > 0) SDL_RenderTexture(ren, tb, 0, &r_bot);
        }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 215); SDL_FRect all = { 0, 0, (float)w, (float)h }; SDL_RenderFillRect(ren, &all);
        float sc = (float)w * 0.84f / (46 * 8), sh = (float)h / (24 * 8 * 1.6f);   /* fit ~46 columns and ~24 rows */
        if (sh < sc) sc = sh; if (sc > 3) sc = 3; if (sc < 1) sc = 1;
        float lh = 8 * sc * 1.6f, x0 = (float)w * 0.08f, y0 = (float)h * 0.06f;
        if (page == 0) {
            text(x0, y0, sc + 1, "Paused", 255, 255, 255);
            for (int i = 0; i < MI_N; i++) {
                char t[96]; item_text(i, t, sizeof t);
                float y = y0 + lh * 2.2f + lh * (float)i;
                if (i == sel) text(x0 - 3 * 8 * sc, y, sc, ">", 255, 230, 80);
                text(x0, y, sc, t, i == sel ? 255 : 210, i == sel ? 230 : 210, i == sel ? 80 : 210);
            }
            text(x0, y0 + lh * (3.4f + MI_N), sc * 0.75f, "Up/Down choose, Left/Right change, Enter select, Esc resume", 160, 160, 160);
        } else {
            text(x0, y0, sc + 1, "Controls", 255, 255, 255);
            for (int i = 0; i < nctl; i++) {
                char t[96];
                if (i < (int)NBINDS) {
                    char keys[64] = ""; for (int j = 0; j < NALT && binds[i][j] != SDL_SCANCODE_UNKNOWN; j++)
                        { if (j) strncat(keys, ", ", sizeof keys - strlen(keys) - 1); strncat(keys, SDL_GetScancodeName(binds[i][j]), sizeof keys - strlen(keys) - 1); }
                    snprintf(t, sizeof t, "%-14s %s", binds_def[i].name, (waiting && i == csel) ? "press a key..." : keys);
                } else snprintf(t, sizeof t, "%-14s %s", "fast_forward", (waiting && i == csel) ? "press a key..." : SDL_GetScancodeName(ff_key));
                float y = y0 + lh * 2.0f + lh * 0.85f * (float)i;
                text(x0, y, sc * 0.85f, t, i == csel ? 255 : 210, i == csel ? 230 : 210, i == csel ? 80 : 210);
            }
            text(x0, y0 + lh * (2.6f + 0.85f * (float)nctl), sc * 0.75f, "Enter: rebind (next key pressed)   Esc: back   Save settings keeps changes", 160, 160, 160);
        }
        { static int test = -1; if (test < 0) test = getenv("RB_MENU_TEST") != 0;      /* test: save one menu frame and exit */
          if (test) { SDL_Surface *sf = SDL_RenderReadPixels(ren, 0); SDL_Surface *c = sf ? SDL_ConvertSurface(sf, SDL_PIXELFORMAT_RGBA32) : 0;
                      if (c) write_png_buf("build/menu_test.png", c->pixels, (uint32_t)c->w, (uint32_t)c->h); exit(0); } }
        if (vkpresent) { SDL_FlushRenderer(ren); vk_show(1, 1); ovl_last[0] = 1; }
        else SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    ff_on = 0; k_set_fast_forward(0);
}

/* Called once per vertical blank with the framebuffers the game currently displays (0 = none yet). */
void video_frame(uint64_t frame, const uint8_t *top, uint32_t top_stride, uint32_t top_fmt,
                 const uint8_t *bot, uint32_t bot_stride, uint32_t bot_fmt) {
    int want_shot = 0;
    for (int i = 0; i < nshots; i++) if (shots[i] == frame) want_shot = 1;
    static int prof = -1; if (prof < 0) prof = getenv("RB_PROFILE_PRESENT") != 0;   /* test: time the window path headless */
    if (headless && !want_shot && !prof) return;
    { struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a); double now = (double)a.tv_sec * 1e3 + (double)a.tv_nsec / 1e6;
      double slept = (double)(k_paced_ns - paced_last) / 1e6; paced_last = k_paced_ns;
      if (frame_end > 0) emu_ms = emu_ms * 0.9 + 0.1 * (now - frame_end - slept); }
    if (prof_left) { if (emu_ms > prof_emu_max) prof_emu_max = emu_ms; if (--prof_left == 0) prof_end(frame); }
    { static long long pa = -2; if (pa == -2) { const char *e = getenv("RB_PROFILE_AT"); pa = e ? atoll(e) : -1; }   /* test hook */
      if (pa >= 0 && (long long)frame == pa && !prof_left) prof_start(frame); }
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
    if (headless && vkpresent) {                           /* test: present a fixed 1280x720 window */
        place_screens(1280, 720, 0);
        cur_scr[0].fb = (uint32_t)(uintptr_t)top; cur_scr[0].native = frame_rgba; cur_scr[0].nw = 400; cur_scr[0].stride = OUT_W * 4;
        cur_scr[1].fb = (uint32_t)(uintptr_t)bot; cur_scr[1].native = frame_rgba + ((size_t)240 * OUT_W + 40) * 4; cur_scr[1].nw = 320; cur_scr[1].stride = OUT_W * 4;
        float rr[2][4] = { { r_top.x, r_top.y, r_top.w, r_top.h }, { r_bot.x, r_bot.y, r_bot.w, r_bot.h } };
        for (int k = 0; k < 2; k++) memcpy(cur_scr[k].rect, rr[k], sizeof rr[k]);
        vkr_present(cur_scr, 1280, 720, 0, 0, 0, 0, fxaa_on);
    }
    if (headless) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) { fprintf(stderr, "window closed\n"); exit(0); }
        if ((e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat) ||
            (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) menu_open = 1;
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.key == SDLK_F1) layout = (layout + 1) % L_COUNT;
            if (e.key.key == SDLK_F2) swapped = !swapped;
            if (ff_toggle && e.key.scancode == ff_key) { ff_on = !ff_on; k_set_fast_forward(ff_on); }
            if (e.key.key == SDLK_F9) show_fps = !show_fps;
            if (e.key.key == SDLK_F10 && !prof_left) prof_start(frame);
            if (e.key.key == SDLK_F5) nfc_host_tap();             /* amiibo: tap the next dump from <game>/amiibo */
            if (e.key.key == SDLK_F6) nfc_host_remove();
            if (e.key.key == SDLK_F11) SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
            if (e.key.key == SDLK_F12) { char p[64]; snprintf(p, sizeof p, "screenshot_%06llu.png", (unsigned long long)frame);
                                         if (hi) write_png_buf(p, frame_hi, OUT_W * hi_scale, OUT_H * hi_scale); else write_png(p); }
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
            pad = SDL_OpenGamepad(e.gdevice.which);
            if (pad) {
                int motion = settings_int("input.motion", 1) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
                if (motion) { SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true); SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true); }
                fprintf(stderr, "gamepad: %s%s\n", SDL_GetGamepadName(pad), motion ? " (motion sensors used for the 3DS gyroscope)" : "");
            }
        }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) { SDL_CloseGamepad(pad); pad = 0; }
    }
    int w, h;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    place_screens(w, h, SDL_GetKeyboardState(0)[SDL_SCANCODE_F3]);
    poll_host_input();
    if (!ff_toggle && ff_key != SDL_SCANCODE_UNKNOWN) { int on = SDL_GetKeyboardState(0)[ff_key]; if (on != ff_on) { ff_on = on; k_set_fast_forward(on); } }
    if (vkpresent) {
        cur_scr[0].fb = (uint32_t)(uintptr_t)top; cur_scr[0].native = frame_rgba; cur_scr[0].nw = 400; cur_scr[0].stride = OUT_W * 4;
        cur_scr[1].fb = (uint32_t)(uintptr_t)bot; cur_scr[1].native = frame_rgba + ((size_t)240 * OUT_W + 40) * 4; cur_scr[1].nw = 320; cur_scr[1].stride = OUT_W * 4;
        ovl_fit(w, h);
        char txt[128] = ""; if (show_fps) fps_text(txt, sizeof txt);
        int dirty = strcmp(txt, ovl_last) != 0;
        if (dirty) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 0); SDL_RenderClear(ren);
            if (*txt) draw_fps(txt);
            SDL_FlushRenderer(ren); snprintf(ovl_last, sizeof ovl_last, "%s", txt);
        }
        struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a);
        vk_show(*txt != 0, dirty);
        struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
        present_ms = present_ms * 0.9 + 0.1 * ((double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6);
        frame_end = (double)b.tv_sec * 1e3 + (double)b.tv_nsec / 1e6;
        { static long long at = -2; if (at == -2) { const char *e = getenv("RB_MENU_TEST"); at = e ? atoll(e) : -1; }
          if (at >= 0 && (long long)frame == at) menu_open = 1; }
        if (menu_open) menu_loop(0, 0);
        return;
    }
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
    if (show_fps) { char txt[128]; fps_text(txt, sizeof txt); draw_fps(txt); }
    { struct timespec a; clock_gettime(CLOCK_MONOTONIC, &a); SDL_RenderPresent(ren); struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
      present_ms = present_ms * 0.9 + 0.1 * ((double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6);
      frame_end = (double)b.tv_sec * 1e3 + (double)b.tv_nsec / 1e6; }
    { static long long at = -2; if (at == -2) { const char *e = getenv("RB_MENU_TEST"); at = e ? atoll(e) : -1; }
      if (at >= 0 && (long long)frame == at) menu_open = 1; }
    if (menu_open) menu_loop(tt, tb);
}
