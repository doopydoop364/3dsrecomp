/* gsp::Gpu service and the GX memory operations (fill, display transfer, texture copy).
 * PICA200 command lists are executed by pica.c.
 * Layouts and algorithms follow the behaviour documented by the Azahar emulator's sources. */
#include "pica.h"
#include "input.h"
#include "vkr.h"
#include <sys/stat.h>
#include <time.h>

void video_frame(uint64_t frame, const uint8_t *top, uint32_t top_stride, uint32_t top_fmt,
                 const uint8_t *bot, uint32_t bot_stride, uint32_t bot_fmt);

enum { IRQ_PSC0, IRQ_PSC1, IRQ_PDC0, IRQ_PDC1, IRQ_PPF, IRQ_P3D, IRQ_DMA };
typedef struct { uint32_t active, left, right, stride, format, shown, unknown; } FbInfo;

static KObject *gsp_mem, *gsp_ev;
static FbInfo shown[2];
static uint64_t frames, swaps, lists, gx_count[8];
static uint64_t ns_gpu, ns_transfer;              /* host time spent executing GPU work */
static uint64_t host_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
uint64_t gpu_frame(void) { return frames; }
uint64_t gpu_swaps(void) { return swaps; }
static uint64_t ns_gpu_vk;                          /* part of ns_gpu spent waiting for the Vulkan GPU / writing back */
void gpu_counters(uint64_t out[3]) { out[0] = ns_gpu; out[1] = ns_transfer; out[2] = ns_gpu_vk; }   /* ns: command lists, display transfers, Vulkan waits inside lists */      /* frames the game finished (buffer swaps), for the FPS counter */
void gpu_report(void);

static void reply(uint32_t *cmd, int normal, int translate) {
    cmd[0] = (cmd[0] & 0xffff0000u) | ((uint32_t)normal << 6) | (uint32_t)translate;
    cmd[1] = RES_OK;
}
static void irq(int id) {
    if (!gsp_mem || !gsp_ev) return;
    uint8_t *q = gsp_mem->mem;                                         /* relay queue of GSP thread 0 */
    if (id == IRQ_PDC0 || id == IRQ_PDC1) {
        if (q[3] & 1) return;                                          /* application ignores vertical blank */
        if (q[1] >= 0x20) { uint32_t *missed = (uint32_t *)(q + (id == IRQ_PDC0 ? 4 : 8)); (*missed)++; return; }
    }
    if (q[1] >= 0x34) { q[2] = 1; return; }
    q[0x0c + (q[0] + q[1]) % 0x34] = (uint8_t)id; q[1]++;
    k_signal(gsp_ev);
}

/* ---- pixel formats: 0 RGBA8, 1 RGB8, 2 RGB565, 3 RGB5A1, 4 RGBA4 ---- */
static const uint32_t bpp_of[8] = { 4, 3, 2, 2, 2, 4, 4, 4 };
typedef struct { uint32_t r, g, b, a; } Px;
static Px px_decode(uint32_t fmt, const uint8_t *p) {
    uint32_t v = (uint32_t)(p[0] | p[1] << 8);
    #define X5(x) (((x) << 3) | ((x) >> 2))
    switch (fmt) {
    case 0: return (Px){ p[3], p[2], p[1], p[0] };
    case 1: return (Px){ p[2], p[1], p[0], 255 };
    case 2: return (Px){ X5(v >> 11), (((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4), X5(v & 31), 255 };
    case 3: return (Px){ X5(v >> 11), X5((v >> 6) & 31), X5((v >> 1) & 31), (v & 1) * 255 };
    default: return (Px){ (v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17 };
    }
    #undef X5
}
static void px_encode(uint32_t fmt, uint8_t *p, Px c) {
    uint32_t v;
    switch (fmt) {
    case 0: p[3] = (uint8_t)c.r; p[2] = (uint8_t)c.g; p[1] = (uint8_t)c.b; p[0] = (uint8_t)c.a; return;
    case 1: p[2] = (uint8_t)c.r; p[1] = (uint8_t)c.g; p[0] = (uint8_t)c.b; return;
    case 2: v = (c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3); break;
    case 3: v = (c.r >> 3) << 11 | (c.g >> 3) << 6 | (c.b >> 3) << 1 | (c.a >> 7); break;
    default: v = (c.r >> 4) << 12 | (c.g >> 4) << 8 | (c.b >> 4) << 4 | (c.a >> 4); break;
    }
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
/* Offset of texel (x, y) inside 8x8 Morton-ordered tiles, in bytes, without the tile-row term. */
static uint32_t morton(uint32_t x, uint32_t y, uint32_t bpp) {
    static const uint32_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    return (xl[x & 7] + yl[y & 7] + (x & ~7u) * 8) * bpp;
}

static void memory_fill(uint32_t start, uint32_t value, uint32_t end, uint32_t ctl) {
    if (end <= start || !mem_valid(start, end - start)) { LOG(1, "gx: memory fill %08x..%08x skipped (bad range)", start, end); return; }
    uint8_t *p = (uint8_t *)(uintptr_t)start, *e = (uint8_t *)(uintptr_t)end;   /* end is exclusive */
    if (ctl & 0x100) for (; p + 3 <= e; p += 3) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); p[2] = (uint8_t)(value >> 16); }
    else if (ctl & 0x200) for (; p + 4 <= e; p += 4) memcpy(p, &value, 4);
    else for (; p + 2 <= e; p += 2) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
}
static void display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags) {
    uint32_t iw = in_dim & 0xffff, ih = in_dim >> 16, ow = out_dim & 0xffff, oh = out_dim >> 16;
    uint32_t ifmt = (flags >> 8) & 7, ofmt = (flags >> 12) & 7, scaling = (flags >> 24) & 3;
    uint32_t flip = flags & 1, in_linear = (flags >> 1) & 1, crop = (flags >> 2) & 1, raw = (flags >> 5) & 1;
    if (!iw || !ih || !ow || !oh || ifmt > 4 || ofmt > 4 || scaling > 2 || (in_linear && scaling)) { LOG(1, "gx: display transfer with unsupported settings (flags %08x) skipped", flags); return; }
    uint32_t ib = bpp_of[ifmt], ob = bpp_of[ofmt], hs = scaling != 0, vs = scaling == 2;
    if (!mem_valid(in, iw * ih * ib) || !mem_valid(out, ow * oh * ob)) { LOG(1, "gx: display transfer %08x -> %08x skipped (bad address)", in, out); return; }
    const uint8_t *src = (const uint8_t *)(uintptr_t)in; uint8_t *dst = (uint8_t *)(uintptr_t)out;
    if (flip && crop) dst += (iw - ow) * (oh - 1) * ob;                /* hardware quirk: skewed output */
    ow >>= hs; oh >>= vs;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t ix = x << hs, iy = y << vs, oy = flip ? oh - y - 1 : y, so, d;
            if (in_linear) { so = (ix + iy * iw) * ib; d = raw ? (x + oy * ow) * ob : morton(x, oy, ob) + (oy & ~7u) * ow * ob; }
            else { so = morton(ix, iy, ib) + (iy & ~7u) * iw * ib; d = raw ? morton(x, oy, ob) + (oy & ~7u) * ow * ob : (x + oy * ow) * ob; }
            Px c = px_decode(ifmt, src + so);
            if (scaling) {                                             /* averages memory-adjacent pixels */
                int n = scaling == 2 ? 4 : 2;
                for (int k = 1; k < n; k++) { Px t = px_decode(ifmt, src + so + (uint32_t)k * ib); c.r += t.r; c.g += t.g; c.b += t.b; c.a += t.a; }
                c.r /= (uint32_t)n; c.g /= (uint32_t)n; c.b /= (uint32_t)n; c.a /= (uint32_t)n;
            }
            px_encode(ofmt, dst + d, c);
        }
}
static void texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_wg, uint32_t out_wg) {
    uint32_t remaining = size & ~15u, igap = (in_wg >> 16) * 16, ogap = (out_wg >> 16) * 16;
    uint32_t iw = igap ? (in_wg & 0xffff) * 16 : remaining, ow = ogap ? (out_wg & 0xffff) * 16 : remaining;
    if (!remaining || !iw || !ow) return;
    if (!mem_valid(in, remaining) || !mem_valid(out, remaining)) { LOG(1, "gx: texture copy %08x -> %08x skipped (bad address)", in, out); return; }
    const uint8_t *s = (const uint8_t *)(uintptr_t)in; uint8_t *d = (uint8_t *)(uintptr_t)out;
    uint32_t il = iw, ol = ow;
    while (remaining) {
        uint32_t n = il < ol ? il : ol; if (n > remaining) n = remaining;
        memmove(d, s, n); s += n; d += n; remaining -= n; il -= n; ol -= n;
        if (!il) { s += igap; il = iw; }
        if (!ol) { d += ogap; ol = ow; }
    }
}

static void run_queue(void) {
    static int log_sync = -1; if (log_sync < 0) log_sync = getenv("RB_LOG_SYNC") != 0;   /* trace GX operations with the surface cache */
    uint8_t *q = gsp_mem->mem + 0x800;
    vkr_new_gen();                                                     /* the guest CPU ran since the last batch */
    while (q[1]) {
        uint32_t c[8];
        memcpy(c, q + 0x20 + (q[0] % 15) * 0x20, sizeof c);
        q[1]--; q[0] = (uint8_t)((q[0] + 1) % 15);
        uint32_t id = c[0] & 0xff;
        if (id < 8) gx_count[id]++;
        switch (id) {
        case 0: vkr_why = 1; vkr_sync_range(c[1], c[3], 0); vkr_why = 1; vkr_sync_range(c[2], c[3], 1);
                if (mem_valid(c[1], c[3]) && mem_valid(c[2], c[3])) memmove((void *)(uintptr_t)c[2], (void *)(uintptr_t)c[1], c[3]); irq(IRQ_DMA); break;
        case 1: { uint64_t vk0[4], vk1[4]; vkr_counters(vk0); uint64_t t0 = host_ns(); uint64_t d0 = pica_draw_count(); lists++; pica_run_list(c[1], c[2]); uint64_t dt = host_ns() - t0; ns_gpu += dt;
                  vkr_counters(vk1); ns_gpu_vk += (vk1[0] - vk0[0]) + (vk1[1] - vk0[1]);
                  { static int64_t tf = -2; if (tf == -2) { const char *e = getenv("RB_TIME_FRAME"); tf = e ? atoll(e) : -1; }
                    if ((int64_t)frames == tf) fprintf(stderr, "gpu: list %08x (%u bytes): %.2f ms, %llu draws\n", c[1], c[2], (double)dt / 1e6, (unsigned long long)(pica_draw_count() - d0)); }
                  irq(IRQ_P3D); break; }
        case 2:
            if (c[1] && !vkr_fill(c[1], c[3], c[2], c[7] & 0xffff)) { vkr_why = 2; vkr_sync_range(c[1], c[3] - c[1], 1); memory_fill(c[1], c[2], c[3], c[7] & 0xffff); }
            if (c[4] && !vkr_fill(c[4], c[6], c[5], c[7] >> 16)) { vkr_why = 2; vkr_sync_range(c[4], c[6] - c[4], 1); memory_fill(c[4], c[5], c[6], c[7] >> 16); }
            if (c[1]) irq(IRQ_PSC0); else if (c[4]) irq(IRQ_PSC1);
            break;
        case 3: { if (log_sync) fprintf(stderr, "xfer: frame %llu %08x -> %08x %ux%u -> %ux%u flags %08x\n", (unsigned long long)frames, c[1], c[2], c[3] & 0xffff, c[3] >> 16, c[4] & 0xffff, c[4] >> 16, c[5]);
                  uint32_t ib = bpp_of[(c[5] >> 8) & 7], ob = bpp_of[(c[5] >> 12) & 7];
                  if (vkr_display_link(c[1], c[2], c[3] & 0xffff, c[3] >> 16, c[4] & 0xffff, c[4] >> 16, c[5])) { irq(IRQ_PPF); break; }   /* shown from the GPU */
                  vkr_why = 3; vkr_sync_range(c[1], (c[3] & 0xffff) * (c[3] >> 16) * ib, 0); vkr_why = 3; vkr_sync_range(c[2], (c[4] & 0xffff) * (c[4] >> 16) * ob, 1); }
                { uint64_t t0 = host_ns(); display_transfer(c[1], c[2], c[3], c[4], c[5]); ns_transfer += host_ns() - t0; irq(IRQ_PPF); break; }
        case 4: { if (log_sync) fprintf(stderr, "copy: frame %llu %08x -> %08x size %x in %08x out %08x\n", (unsigned long long)frames, c[1], c[2], c[3], c[4], c[5]);
                  uint32_t span = c[3] * 2 + 0x100000; vkr_why = 4; vkr_sync_range(c[1], span, 0); vkr_why = 4; vkr_sync_range(c[2], span, 1); }   /* gaps: generous span */
                texture_copy(c[1], c[2], c[3], c[4], c[5]); irq(IRQ_PPF); break;
        case 5: break;
        default: LOG(1, "gx: unknown command %u", id);
        }
    }
}
void control_vblank(uint64_t frame);
void k_restart_threads(void);
static void vblank(void) {
    frames++;
    { static int64_t dump_at = -2; if (dump_at == -2) { const char *s = getenv("RB_MEMDUMP_FRAME"); dump_at = s ? atoll(s) : -1; }
      if ((int64_t)frames == dump_at) mem_dump_all("build/memdump"); }
    if (gsp_mem)
        for (int s = 0; s < 2; s++) {                                  /* pick up a framebuffer swap, if the game queued one */
            uint8_t *u = gsp_mem->mem + 0x200 + s * 0x40;
            if (u[1]) { memcpy(&shown[s], u + 4 + (u[0] & 1) * 0x1c, sizeof(FbInfo)); u[1] = 0; if (s == 0) swaps++; }
        }
    const uint8_t *fb[2] = { 0, 0 };
    for (int s = 0; s < 2; s++) if (shown[s].left) { vkr_why = 5; vkr_sync_range(shown[s].left, shown[s].stride * (s ? 320u : 400u), 0); }
    vkr_frame();
    for (int s = 0; s < 2; s++)
        if (shown[s].left && mem_valid(shown[s].left, shown[s].stride * (s ? 320u : 400u))) fb[s] = (const uint8_t *)(uintptr_t)shown[s].left;
    video_frame(frames, fb[0], shown[0].stride, shown[0].format, fb[1], shown[1].stride, shown[1].format);
    input_frame(frames);
    { static long long from = -2; if (from == -2) { const char *e = getenv("RB_LOG_FROM"); from = e ? atoll(e) : -1; }
      if (from >= 0 && (long long)frames == from) log_level = getenv("RB_LOG") ? atoi(getenv("RB_LOG")) : 3; }
    if (frames % 300 == 0) {
        static uint64_t last_swaps, last_lists, last_draws, last_wall, last_gpu, last_tr;
        uint64_t wall = host_ns();
        if (last_wall) LOG(1, "gpu: host time for the last 300 frames: %.2f s total, %.2f s command lists, %.2f s display transfers",
                           (double)(wall - last_wall) / 1e9, (double)(ns_gpu - last_gpu) / 1e9, (double)(ns_transfer - last_tr) / 1e9);
        last_wall = wall; last_gpu = ns_gpu; last_tr = ns_transfer;
        uint64_t draws = pica_draw_count();
        LOG(1, "gpu: frame %llu: last 5 s had %.1f buffer swaps/s, %.1f command lists and %.1f draw calls per swap",
            (unsigned long long)frames, (double)(swaps - last_swaps) / 5.0,
            swaps > last_swaps ? (double)(lists - last_lists) / (double)(swaps - last_swaps) : 0.0,
            swaps > last_swaps ? (double)(draws - last_draws) / (double)(swaps - last_swaps) : 0.0);
        last_swaps = swaps; last_lists = lists; last_draws = draws;
    }
    irq(IRQ_PDC0); irq(IRQ_PDC1);
    { static int64_t exit_at = -2; if (exit_at == -2) { const char *e = getenv("RB_EXIT_FRAME"); exit_at = e ? atoll(e) : -1; }
      if ((int64_t)frames == exit_at) { gpu_report(); exit(0); } }   /* tests: stop after a fixed frame */
    { static int64_t rs = -2; if (rs == -2) { const char *e = getenv("RB_TEST_RESUME"); rs = e ? atoll(e) : -1; }
      if ((int64_t)frames == rs) k_restart_threads(); }              /* test: continue every thread from its registers */
    control_vblank(frames);                                            /* RB_CONTROL: wait for the next command */
}

void gsp_service(KObject *s, uint32_t *cmd) {
    switch (cmd[0] >> 16) {
    case 0x0013:                                                       /* RegisterInterruptRelayQueue */
        gsp_ev = h_get(cmd[3]);
        if (!gsp_mem) { gsp_mem = k_shmem(0x1000); k_add_periodic(vblank, 16715000); }
        memset(gsp_mem->mem, 0, 12);
        reply(cmd, 2, 2); cmd[1] = 0x2a07; cmd[2] = 0; cmd[3] = 0; cmd[4] = h_new(gsp_mem);
        break;
    case 0x000c:                                                       /* TriggerCmdReqQueue */
        if (gsp_mem) {
            static int64_t log_frame = -2; static int trig;
            static uint64_t trig_frame = ~0ull;
            if (log_frame == -2) { const char *e = getenv("RB_GX_LOG"); log_frame = e ? atoll(e) : -1; }
            if ((int64_t)frames == log_frame) {
                if (trig_frame != frames) { trig_frame = frames; trig = 0; mkdir("build/replay", 0755); }
                uint8_t *q = gsp_mem->mem + 0x800;
                fprintf(stderr, "gx: frame %llu trigger %d from guest %08x (lr %08x), %u commands queued at index %u\n",
                        (unsigned long long)frames, trig, cur->cpu.r[15], cur->cpu.r[14], q[1], q[0]);
                for (int k = 0; k < q[1]; k++) {
                    uint32_t c[8]; memcpy(c, q + 0x20 + ((q[0] + k) % 15) * 0x20, sizeof c);
                    fprintf(stderr, "gx:   cmd %u: %08x %08x %08x %08x %08x %08x %08x\n", c[0] & 0xff, c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
                }
                char p[64]; snprintf(p, sizeof p, "build/replay/state_%02d.bin", trig);
                pica_save_state(p);
                { uint32_t c[8]; memcpy(c, q + 0x20 + (q[0] % 15) * 0x20, sizeof c);
                  if ((c[0] & 0xff) == 1) { FILE *lf = fopen("build/replay/frame_lists.txt", trig ? "a" : "w"); if (lf) { fprintf(lf, "%08x %x\n", c[1], c[2]); fclose(lf); } } }
                trig++;
            }
            run_queue();
        }
        reply(cmd, 1, 0); break;
    case 0x0005: if (cmd[1] < 2) { memcpy(&shown[cmd[1]], &cmd[2], sizeof(FbInfo)); if (cmd[1] == 0) swaps++; } reply(cmd, 1, 0); break;   /* SetBufferSwap */
    case 0x0001: case 0x0002: case 0x0008: case 0x000b: case 0x0016: case 0x0017: case 0x001f:
        LOG(2, "gsp: cmd %08x %08x %08x %08x", cmd[0], cmd[1], cmd[2], cmd[3]);
        reply(cmd, 1, 0); break;                                       /* register writes, cache flush, LCD, rights */
    default:
        LOG(1, "STUB %-9s cmd %08x  args %08x %08x %08x %08x", s->name, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
        reply(cmd, 1, 0);
    }
}
/* Summary for the end of a run. */
void gpu_report(void) {
    fprintf(stderr, "gpu: %llu frames, %llu swaps, %llu command lists; GX commands: dma %llu, lists %llu, fills %llu, transfers %llu, texture copies %llu\n",
            (unsigned long long)frames, (unsigned long long)swaps, (unsigned long long)lists,
            (unsigned long long)gx_count[0], (unsigned long long)gx_count[1], (unsigned long long)gx_count[2], (unsigned long long)gx_count[3], (unsigned long long)gx_count[4]);
    pica_report();
}
