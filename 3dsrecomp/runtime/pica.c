/* PICA200 GPU: register file, command lists, vertex loading, primitive assembly, clipping and a
 * software rasteriser. Behaviour follows the facts in docs/pica_facts.md.
 * RB_PICA_LOG=n logs the first n draws in detail. */
#include "pica.h"
#include "vkr.h"
#include <math.h>
#include <pthread.h>
#include <unistd.h>

static uint32_t reg[0x300];
static uint32_t lut[24][256]; static uint64_t lut_version;   /* lighting lookup tables, filled through 0x1C8-0x1CF */
static uint32_t fog_lut[128];               /* fog factors, filled through 0x0E8-0x0EF at index 0x0E6 */
static uint32_t pt_lut[6][256];             /* procedural texture: noise, -, colour map, alpha map, colour, colour delta
                                             * (filled through 0x0B0-0x0B7, table and index in 0x0AF) */
static uintptr_t cur_word;
static uint32_t top_list, cur_list;         /* top-level list of this submission, and the list now executing */                 /* host address of the command word being executed */
static int gs_b15;
/* Immediate mode (0x232 = 15): attributes arrive one by one through 0x233-0x235; each complete vertex (0x242 + 1
 * attributes) is shaded and fed to a primitive assembler that lasts across vertices (0x25e topology, 0x25f restart);
 * triangles are drawn as they complete, as Azahar's DrawImmediate does. */
static float imm_attr[16][4]; static uint32_t imm_cur; static int imm_nb, imm_odd;
static void draw_immediate(void);                          /* b15 of the geometry shader reads 1 after any invocation, until 0x280 is written */
static uintptr_t uni_src[96];              /* where each uniform's last upload came from */
static PicaShader vs, gs;                  /* vertex and geometry shader units */
static uint64_t vs_ver = 1, gs_ver = 1;     /* bumped on every program or swizzle write */
static uint32_t uni_queue[4]; static int uni_n;
static uint32_t gs_uni_queue[4]; static int gs_uni_n;
static float dflt[16][4]; static uint32_t dflt_queue[3]; static int dflt_n;
static uint64_t n_partial_uni, n_f32_uni, n_f24_uni;
static uint64_t t_shade, t_assemble, t_raster, t_setup;
static uint64_t pns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t n_in_tris, n_clipped, n_culled, n_trivial;
static uint64_t t_pool_wait;                 /* main thread waiting for shading workers */
static int dump_draw = -1, dump_left;
static float ndc_min[2], ndc_max[2];
static uint64_t vtx_out[7], vtx_total, frag_cov, frag_alpha, frag_depth, frag_stencil;
static uint64_t n_fog_draws, n_draws, n_tris, n_pixels, n_lists, n_skipped_gs, reg_writes[0x300];
static int log_draws = -1;
static int64_t log_from = -1, log_frame = -1;
static uint64_t frame_draw_base, frame_seen = ~0ull;

uint64_t pica_draw_count(void) { return n_draws; }
static uint64_t n_shaded;                       /* vertices run through the vertex shader */
/* Running totals for the in-game profiler (F10): ns in draw setup, vertex shading, assembly+clipping, rasterisation
 * (software) or Vulkan recording; then draws, triangles, shaded vertices. */
void pica_counters(uint64_t out[9]) { out[0] = t_setup; out[1] = t_shade; out[2] = t_assemble; out[3] = t_raster; out[4] = n_draws; out[5] = n_tris; out[6] = n_shaded;
                                       out[7] = t_pool_wait; out[8] = n_trivial; }

/* ---- addresses: everything inside command lists and GPU registers is physical ---- */
static uint8_t *phys(uint32_t p, uint32_t size) {
    uint32_t v;
    if (p - 0x18000000u < 0x00600000u) v = p - 0x18000000u + 0x1f000000u;           /* VRAM */
    else if (p - 0x20000000u < 0x08000000u) v = p - 0x20000000u + LINEAR_BASE;      /* FCRAM, linear mapping */
    else return 0;
    return mem_valid(v, size ? size : 1) ? (uint8_t *)(uintptr_t)v : 0;
}

static float f24(uint32_t raw) {
    raw &= 0xffffff;
    uint32_t sign = raw >> 23, exp = (raw >> 16) & 0x7f, man = raw & 0xffff, bits;
    if (exp == 0 && man == 0) bits = sign << 31;
    else if (exp == 0x7f) bits = sign << 31 | 0xffu << 23 | man << 7;
    else if (exp == 0) { float f = ldexpf((float)man / 65536.0f, -62); return sign ? -f : f; }
    else bits = sign << 31 | (exp + 64) << 23 | man << 7;
    float f; memcpy(&f, &bits, 4); return f;
}
static void unpack24(const uint32_t *b, float out[4]) {               /* 3-word float24 vector: x y z w */
    out[0] = f24(b[2]);
    out[1] = f24(((b[1] & 0xffff) << 8) | (b[2] >> 24));
    out[2] = f24(((b[0] & 0xff) << 16) | (b[1] >> 16));
    out[3] = f24(b[0] >> 8);
}

static void draw(int indexed);

/* Returns a jump target for the command-list processor (0 = none). */
static uint32_t write_reg(uint32_t id, uint32_t value, uint32_t mask) {
    if (id >= 0x300) return 0;
    uint32_t m = (mask & 1 ? 0xffu : 0) | (mask & 2 ? 0xff00u : 0) | (mask & 4 ? 0xff0000u : 0) | (mask & 8 ? 0xff000000u : 0);
    uint32_t v = (reg[id] & ~m) | (value & m);
    reg[id] = v;
    reg_writes[id]++;
    switch (id) {
    case 0x22e: draw(0); break;
    case 0x22f: draw(1); break;
    case 0x232: dflt_n = 0; imm_cur = 0; break;
    case 0x25e: case 0x25f: imm_nb = 0; imm_odd = 0; break;           /* topology / restart: a new primitive (immediate mode) */
    case 0x280: gs_b15 = 0; break;                                   /* the program sets b0-b15: b15 is its own again */
    case 0x233: case 0x234: case 0x235:                              /* fixed vertex attribute data */
        dflt_queue[dflt_n++] = v;
        if (dflt_n == 3) {
            uint32_t idx = reg[0x232] & 0xf;
            if (idx < 15) unpack24(dflt_queue, dflt[idx]);
            else {                                                   /* index 15: immediate mode, a vertex attribute */
                unpack24(dflt_queue, imm_attr[imm_cur]);
                if (imm_cur < (reg[0x242] & 15)) imm_cur++;
                else { imm_cur = 0; draw_immediate(); }
                dflt_n = 0;
                break;
            }
            if (getenv("RB_LOG_FIXED") && (int64_t)gpu_frame() == atoll(getenv("RB_LOG_FIXED")))
                fprintf(stderr, "  fixed attr write: reg232 %08x -> attr %u = (%g %g %g %g) raw %08x %08x %08x\n", reg[0x232], idx,
                        dflt[idx < 15 ? idx : 0][0], dflt[idx < 15 ? idx : 0][1], dflt[idx < 15 ? idx : 0][2], dflt[idx < 15 ? idx : 0][3], dflt_queue[0], dflt_queue[1], dflt_queue[2]);
            reg[0x232] = (reg[0x232] & ~0xfu) | ((idx + 1) & 0xf);
            dflt_n = 0;
        }
        break;
    case 0x23c: case 0x23d: {                                        /* jump to another command list */
        int k = (int)(id - 0x23c);
        return 0x80000000u | (uint32_t)k;
    }
    case 0x1c8: case 0x1c9: case 0x1ca: case 0x1cb: case 0x1cc: case 0x1cd: case 0x1ce: case 0x1cf: {
        uint32_t cfg = reg[0x1c5], idx = cfg & 0xff, type = (cfg >> 8) & 0x1f;
        if (type < 24) { lut[type][idx] = v; lut_version++; }
        reg[0x1c5] = (cfg & ~0xffu) | ((idx + 1) & 0xff);
        break;
    }
    case 0x0b0: case 0x0b1: case 0x0b2: case 0x0b3: case 0x0b4: case 0x0b5: case 0x0b6: case 0x0b7: {
        uint32_t cfg = reg[0x0af], idx = cfg & 0xff, t = (cfg >> 8) & 15;
        if (t < 6) { pt_lut[t][idx] = v; lut_version++; }
        reg[0x0af] = (cfg & ~0xffu) | ((idx + 1) & 0xff);
        break;
    }
    case 0x0e8: case 0x0e9: case 0x0ea: case 0x0eb: case 0x0ec: case 0x0ed: case 0x0ee: case 0x0ef:
        fog_lut[reg[0x0e6] % 128] = v; reg[0x0e6] = (reg[0x0e6] + 1) & 0xffff; lut_version++;
        break;
    case 0x2c0: if (uni_n) n_partial_uni++; uni_n = 0; if (v >> 31) n_f32_uni++; else n_f24_uni++; break;
    case 0x2c1: case 0x2c2: case 0x2c3: case 0x2c4: case 0x2c5: case 0x2c6: case 0x2c7: case 0x2c8: {
        int f32 = (int)(reg[0x2c0] >> 31);
        uni_queue[uni_n++] = v;
        if (uni_n == (f32 ? 4 : 3)) {
            uint32_t idx = reg[0x2c0] & 0x7f;
            if (idx < 96) {
                uni_src[idx] = cur_word;
                if (f32) for (int i = 0; i < 4; i++) memcpy(&vs.uniform[idx][3 - i], &uni_queue[i], 4);
                else unpack24(uni_queue, vs.uniform[idx]);
            }
            reg[0x2c0] = (reg[0x2c0] & ~0x7fu) | ((idx + 1) & 0x7f);
            uni_n = 0;
        }
        break;
    }
    case 0x2cc: case 0x2cd: case 0x2ce: case 0x2cf: case 0x2d0: case 0x2d1: case 0x2d2: case 0x2d3: {
        uint32_t o = reg[0x2cb];
        if (o < 512) { vs.code[o] = v; vs_ver++; if (!(reg[0x244] & 1)) { gs.code[o] = v; gs_ver++; } }   /* units not configured apart: shared */
        reg[0x2cb] = o + 1;
        break;
    }
    case 0x2d6: case 0x2d7: case 0x2d8: case 0x2d9: case 0x2da: case 0x2db: case 0x2dc: case 0x2dd: {
        uint32_t o = reg[0x2d5];
        if (o < 128) { vs.desc[o] = v; vs_ver++; if (!(reg[0x244] & 1)) { gs.desc[o] = v; gs_ver++; } }
        reg[0x2d5] = o + 1;
        break;
    }
    /* geometry shader unit: the same layout as the vertex shader's registers, 0x30 lower */
    case 0x290: gs_uni_n = 0; break;
    case 0x291: case 0x292: case 0x293: case 0x294: case 0x295: case 0x296: case 0x297: case 0x298: {
        int f32 = (int)(reg[0x290] >> 31);
        gs_uni_queue[gs_uni_n++] = v;
        if (gs_uni_n == (f32 ? 4 : 3)) {
            uint32_t idx = reg[0x290] & 0x7f;
            if (idx < 96) {
                if (f32) for (int i = 0; i < 4; i++) memcpy(&gs.uniform[idx][3 - i], &gs_uni_queue[i], 4);
                else unpack24(gs_uni_queue, gs.uniform[idx]);
            }
            reg[0x290] = (reg[0x290] & ~0x7fu) | ((idx + 1) & 0x7f);
            gs_uni_n = 0;
        }
        break;
    }
    case 0x29c: case 0x29d: case 0x29e: case 0x29f: case 0x2a0: case 0x2a1: case 0x2a2: case 0x2a3: {
        uint32_t o = reg[0x29b];
        if (o < 512) { gs.code[o] = v; gs_ver++; }
        reg[0x29b] = o + 1;
        break;
    }
    case 0x2a6: case 0x2a7: case 0x2a8: case 0x2a9: case 0x2aa: case 0x2ab: case 0x2ac: case 0x2ad: {
        uint32_t o = reg[0x2a5];
        if (o < 128) { gs.desc[o] = v; gs_ver++; }
        reg[0x2a5] = o + 1;
        break;
    }
    default: break;
    }
    return 0;
}

void pica_run_list(uint32_t vaddr, uint32_t size) {
    if (!mem_valid(vaddr, size)) { LOG(1, "pica: command list at %08x (%u bytes) is not in mapped memory", vaddr, size); return; }
    const uint32_t *w = (const uint32_t *)(uintptr_t)vaddr;
    top_list = cur_list = vaddr;
    uint32_t n = size / 4, i = 0;
    n_lists++;
    /* Games chain many small lists with jumps (Robobot's 3D scene uses hundreds per pass); the cap only
     * stops a list that jumps to itself forever. */
    for (int jumps = 0; jumps < 1000000;) {
        uint32_t jump = 0;
        while (i + 1 < n && !jump) {
            uint32_t value = w[i++], hdr = w[i++], id = hdr & 0xffff, mask = (hdr >> 16) & 15, extra = (hdr >> 20) & 0xff;
            cur_word = (uintptr_t)&w[i - 2];
            jump = write_reg(id, value, mask);
            for (uint32_t k = 0; k < extra && i < n && !jump; k++) cur_word = (uintptr_t)&w[i], jump = write_reg((hdr >> 31) ? id + 1 + k : id, w[i++], mask);
            if (i & 1) i++;
        }
        if (!jump) return;
        if (jumps == 999999) { LOG(1, "pica: command list chain from %08x did not end after 1000000 jumps", top_list); return; }
        int k = (int)(jump & 1);
        uint32_t addr = (reg[0x23a + k] & 0x0fffffff) << 3, bytes = (reg[0x238 + k] & 0xfffff) << 3;
        if (getenv("RB_LOG_JUMPS")) fprintf(stderr, "pica: list %08x jumps (channel %d) to physical %08x, %u bytes\n", cur_list, k, addr, bytes);
        const uint8_t *p = phys(addr, bytes);
        if (!p) { LOG(1, "pica: jump to command list at physical %08x is not mapped", addr); return; }
        w = (const uint32_t *)p; n = bytes / 4; i = 0; jumps++; cur_list = addr;
    }
}

/* ---- vertex pipeline ---- */
enum { S_POS = 0, S_QUAT = 4, S_COL = 8, S_TC0 = 12, S_TC1 = 14, S_TC0W = 16, S_VIEW = 18, S_TC2 = 22, NSEM = 24 };
typedef struct { float a[NSEM]; float invw; int32_t x, y; float z; } Vtx;

static void load_vertex(uint32_t base, uint32_t vertex, float in[16][4]) {
    uint32_t a201 = reg[0x201], a202 = reg[0x202];
    uint32_t nattr = (a202 >> 28) + 1, fixed_mask = (a202 >> 16) & 0xfff;
    uint32_t src[12] = { 0 }, stride[12] = { 0 }, have = 0;
    for (int L = 0; L < 12; L++) {
        uint32_t off = reg[0x203 + 3 * L] & 0x0fffffff, A = reg[0x204 + 3 * L], B = reg[0x205 + 3 * L];
        uint32_t ncomp = B >> 28, st = (B >> 16) & 0xff, run = 0;
        for (uint32_t c = 0; c < ncomp; c++) {
            uint32_t code = c < 8 ? (A >> (4 * c)) & 15 : (B >> (4 * (c - 8))) & 15;
            if (code < 12) {
                uint32_t f = code < 8 ? (a201 >> (4 * code)) & 15 : (a202 >> (4 * (code - 8))) & 15;
                uint32_t esize = (f & 3) == 3 ? 4 : (f & 3) == 2 ? 2 : 1, cnt = (f >> 2) + 1;
                run = (run + esize - 1) & ~(esize - 1);
                src[code] = off + run; stride[code] = st; have |= 1u << code;
                run += cnt * esize;
            } else run = ((run + 3) & ~3u) + (code - 11) * 4;
        }
    }
    float attr[16][4];
    for (uint32_t a = 0; a < 16; a++) {
        if (a >= nattr) break;
        if (a >= 12 || (fixed_mask >> a) & 1 || !((have >> a) & 1)) { memcpy(attr[a], dflt[a], sizeof attr[a]); continue; }
        uint32_t f = a < 8 ? (a201 >> (4 * a)) & 15 : (a202 >> (4 * (a - 8))) & 15, type = f & 3, cnt = (f >> 2) + 1;
        uint32_t esize = type == 3 ? 4 : type == 2 ? 2 : 1;
        const uint8_t *p = phys(base + src[a] + stride[a] * vertex, cnt * esize);
        for (uint32_t c = 0; c < 4; c++) {
            if (c >= cnt || !p) { attr[a][c] = c == 3 ? 1.0f : 0.0f; continue; }
            const uint8_t *e = p + c * esize;
            switch (type) {
            case 0: attr[a][c] = (float)(int8_t)e[0]; break;
            case 1: attr[a][c] = (float)e[0]; break;
            case 2: attr[a][c] = (float)(int16_t)(e[0] | e[1] << 8); break;
            default: memcpy(&attr[a][c], e, 4);
            }
        }
    }
    uint32_t nin = (reg[0x2b9] & 15) + 1;
    uint64_t map = (uint64_t)reg[0x2bc] << 32 | reg[0x2bb];
    for (uint32_t a = 0; a < nin && a < nattr; a++) memcpy(in[(map >> (4 * a)) & 15], attr[a], sizeof attr[a]);
}

static const uint32_t tev_reg[6];
static void shade_vertex(uint32_t base, uint32_t vertex, Vtx *out);
static void dump_vertex(uint32_t base, uint32_t vertex, const Vtx *v) {
    float in[16][4] = { { 0 } };
    load_vertex(base, vertex, in);
    uint32_t nin = (reg[0x2b9] & 15) + 1;
    static int once;
    if (!once++) {
        fprintf(stderr, "  base %08x map %08x %08x inputs %u\n", (reg[0x200] & 0x1ffffffe) << 3, reg[0x2bb], reg[0x2bc], (reg[0x2b9] & 15) + 1);
        for (int L = 0; L < 12; L++) if (reg[0x205 + 3 * L] >> 28) fprintf(stderr, "  loader %d: offset %08x comps %08x %08x\n", L, reg[0x203 + 3 * L], reg[0x204 + 3 * L], reg[0x205 + 3 * L]);
        for (int i = 0; i < 16; i++) fprintf(stderr, "  fixed attr %d = (%g %g %g %g)\n", i, dflt[i][0], dflt[i][1], dflt[i][2], dflt[i][3]);
        static const uint32_t tb[3] = { 0x081, 0x091, 0x099 }, tf[3] = { 0x08e, 0x096, 0x09e };
        fprintf(stderr, "  texture config %08x, output semantics %08x %08x %08x %08x %08x %08x %08x\n", reg[0x080],
                reg[0x50], reg[0x51], reg[0x52], reg[0x53], reg[0x54], reg[0x55], reg[0x56]);
        for (int u = 0; u < 3; u++)
            fprintf(stderr, "  tex%d: %ux%u fmt %u wrap/filter %08x lod %08x addr %08x border %08x\n", u, (reg[tb[u] + 1] >> 16) & 0x7ff, reg[tb[u] + 1] & 0x7ff,
                    reg[tf[u]] & 15, reg[tb[u] + 2], reg[tb[u] + 3], (reg[tb[u] + 4] & 0x0fffffff) << 3, reg[tb[u]]);
        for (int i = 0; i < 6; i++) { uint32_t b = tev_reg[i];
            fprintf(stderr, "  tev%d: src %08x opd %08x op %08x const %08x scale %08x\n", i, reg[b], reg[b + 1], reg[b + 2], reg[b + 3], reg[b + 4]); }
        fprintf(stderr, "  tev buffer cfg %08x color %08x | blend %08x %08x | alpha test %08x\n", reg[0x0e0], reg[0x0fd], reg[0x100], reg[0x101], reg[0x104]);
    }
    static int saved;
    if (!saved++ || getenv("RB_DUMP_ALWAYS")) {                                                    /* shader, uniforms and inputs for tools/shader_trace.py */
        FILE *f = fopen("build/shader_dump.bin", "wb");
        uint32_t hdr[8] = { reg[0x2ba] & 0xffff, reg[0x2b0] & 0xffff, reg[0x2b1], reg[0x2b2], reg[0x2b3], reg[0x2b4], reg[0x2bd] & 0xffff, reg[0x04f] & 7 };
        if (f) { fwrite(hdr, 4, 8, f); fwrite(vs.code, 4, 512, f); fwrite(vs.desc, 4, 128, f); fwrite(vs.uniform, 4, 96 * 4, f);
                 fwrite(in, 4, 64, f); fwrite(&reg[0x050], 4, 7, f); fclose(f); }
    }
    fprintf(stderr, "  vertex %u: attr cfg %08x %08x, inputs:", vertex, reg[0x201], reg[0x202]);
    for (uint32_t r = 0; r < nin && r < 16; r++) fprintf(stderr, " v%u(%g %g %g %g)", r, in[r][0], in[r][1], in[r][2], in[r][3]);
    fprintf(stderr, "\n    out pos (%g %g %g %g) col (%g %g %g %g) tc0 (%g %g)\n", v->a[0], v->a[1], v->a[2], v->a[3], v->a[8], v->a[9], v->a[10], v->a[11], v->a[12], v->a[13]);
}
/* Output registers of the last shader stage (enabled by omask, in register order) -> vertex attributes, through
 * the output semantic map 0x050-0x056. */
static void map_outputs(const float o[16][4], uint32_t omask, Vtx *out) {
    for (int i = 0; i < NSEM; i++) out->a[i] = 1.0f;
    uint32_t total = reg[0x04f] & 7, slot = 0;
    for (uint32_t r = 0; r < 16 && slot < total; r++) {
        if (!((omask >> r) & 1)) continue;
        uint32_t sem = reg[0x050 + slot];
        for (int c = 0; c < 4; c++) { uint32_t s = (sem >> (8 * c)) & 0x1f; if (s < NSEM) out->a[s] = o[r][c]; }
        slot++;
    }
    for (int c = 0; c < 4; c++) { float v = fabsf(out->a[S_COL + c]); out->a[S_COL + c] = v < 1.0f ? v : 1.0f; }
}
static void shade_vertex(uint32_t base, uint32_t vertex, Vtx *out) {
    float in[16][4] = { { 0 } }, o[16][4] = { { 0 } };
    load_vertex(base, vertex, in);
    if (pica_shader_run(&vs, reg[0x2ba] & 0xffff, in, o) < 0) LOG(1, "pica: vertex shader did not finish");
    map_outputs(o, reg[0x2bd] & 0xffff, out);
}

/* ---- framebuffer access ---- */
static const uint32_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
typedef struct { uint8_t *color, *depth; uint32_t width, hreg, cfmt, dfmt, cbpp, dbpp; } Fb;
static uint32_t fb_off(const Fb *fb, uint32_t x, uint32_t y, uint32_t bpp) {
    uint32_t yy = fb->hreg - y;
    return (xl[x & 7] + yl[yy & 7] + (x & ~7u) * 8) * bpp + (yy & ~7u) * fb->width * bpp;
}
static void color_write(uint8_t *p, uint32_t fmt, const uint8_t c[4]) {
    uint32_t v;
    switch (fmt) {
    case 0: p[0] = c[3]; p[1] = c[2]; p[2] = c[1]; p[3] = c[0]; return;
    case 1: p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; return;
    case 2: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 3) << 6 | (uint32_t)(c[2] >> 3) << 1 | (uint32_t)(c[3] >> 7); break;
    case 3: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | (uint32_t)(c[2] >> 3); break;
    default: v = (uint32_t)(c[0] >> 4) << 12 | (uint32_t)(c[1] >> 4) << 8 | (uint32_t)(c[2] >> 4) << 4 | (uint32_t)(c[3] >> 4); break;
    }
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static int compare(uint32_t f, uint32_t a, uint32_t b) {
    switch (f) { case 0: return 0; case 1: return 1; case 2: return a == b; case 3: return a != b;
                 case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b; }
}

/* ---- textures ---- */
static const uint8_t tex_bits[16] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0 };
static const int etc_mod[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };
typedef struct { const uint8_t *base; uint32_t w, h, fmt, wrap_s, wrap_t, border; int on; uint32_t param, lod; int cube; const uint8_t *face[6]; } TexUnit;
static int warned_tex[8];

static uint8_t x4(uint32_t v) { return (uint8_t)((v << 4) | v); }
static uint8_t x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static void etc1(const uint8_t *blk, const uint8_t *ablk, uint32_t lx, uint32_t ly, uint8_t out[4]) {
    uint64_t b, a = 0;
    memcpy(&b, blk, 8);
    if (ablk) memcpy(&a, ablk, 8);
    uint32_t texel = lx * 4 + ly, flip = (b >> 32) & 1, diff = (b >> 33) & 1;
    int second = flip ? ly >= 2 : lx >= 2;
    uint32_t table = second ? (b >> 34) & 7 : (b >> 37) & 7;
    int base[3];
    if (diff) {
        int r = (b >> 59) & 31, g = (b >> 51) & 31, bl = (b >> 43) & 31;
        if (second) {
            r += ((int)((b >> 56) & 7) ^ 4) - 4; g += ((int)((b >> 48) & 7) ^ 4) - 4; bl += ((int)((b >> 40) & 7) ^ 4) - 4;
        }
        base[0] = x5((uint32_t)r & 31); base[1] = x5((uint32_t)g & 31); base[2] = x5((uint32_t)bl & 31);
    } else {
        base[0] = x4(second ? (b >> 56) & 15 : (b >> 60) & 15);
        base[1] = x4(second ? (b >> 48) & 15 : (b >> 52) & 15);
        base[2] = x4(second ? (b >> 40) & 15 : (b >> 44) & 15);
    }
    int mod = etc_mod[table][(b >> texel) & 1];
    if ((b >> (16 + texel)) & 1) mod = -mod;
    for (int i = 0; i < 3; i++) { int v = base[i] + mod; out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    out[3] = ablk ? x4((uint32_t)(a >> (4 * texel)) & 15) : 255;
}
static void texel(const TexUnit *t, uint32_t x, uint32_t y, uint8_t out[4]) {
    uint32_t tile = 8 * tex_bits[t->fmt];                             /* bytes per 8x8 tile */
    const uint8_t *tp = t->base + ((y >> 3) * (t->w >> 3) + (x >> 3)) * tile;
    uint32_t fx = x & 7, fy = y & 7, mi = xl[fx] + yl[fy];
    if (t->fmt == 12 || t->fmt == 13) {
        uint32_t sub = (fx >> 2) + 2 * (fy >> 2), sz = t->fmt == 13 ? 16 : 8;
        const uint8_t *blk = tp + sub * sz;
        etc1(t->fmt == 13 ? blk + 8 : blk, t->fmt == 13 ? blk : 0, fx & 3, fy & 3, out);
        return;
    }
    const uint8_t *p = tp + mi * tex_bits[t->fmt] / 8;
    uint32_t v = (uint32_t)(p[0] | (tex_bits[t->fmt] > 8 ? p[1] << 8 : 0));
    switch (t->fmt) {
    case 0: out[0] = p[3]; out[1] = p[2]; out[2] = p[1]; out[3] = p[0]; break;
    case 1: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255; break;
    case 2: out[0] = x5(v >> 11); out[1] = x5((v >> 6) & 31); out[2] = x5((v >> 1) & 31); out[3] = (v & 1) ? 255 : 0; break;
    case 3: out[0] = x5(v >> 11); out[1] = (uint8_t)((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4)); out[2] = x5(v & 31); out[3] = 255; break;
    case 4: out[0] = x4(v >> 12); out[1] = x4((v >> 8) & 15); out[2] = x4((v >> 4) & 15); out[3] = x4(v & 15); break;
    case 5: out[0] = out[1] = out[2] = p[1]; out[3] = p[0]; break;
    case 6: out[0] = p[1]; out[1] = p[0]; out[2] = 0; out[3] = 255; break;
    case 7: out[0] = out[1] = out[2] = p[0]; out[3] = 255; break;
    case 8: out[0] = out[1] = out[2] = 0; out[3] = p[0]; break;
    case 9: out[0] = out[1] = out[2] = x4(p[0] >> 4); out[3] = x4(p[0] & 15); break;
    case 10: { uint32_t n = (mi & 1) ? p[0] >> 4 : p[0] & 15; out[0] = out[1] = out[2] = x4(n); out[3] = 255; break; }
    default: { uint32_t n = (mi & 1) ? p[0] >> 4 : p[0] & 15; out[0] = out[1] = out[2] = 0; out[3] = x4(n); break; }
    }
}
static int wrap(int32_t v, uint32_t size, uint32_t mode) {
    switch (mode) {
    case 0: return v < 0 ? 0 : v >= (int32_t)size ? (int)size - 1 : v;
    case 3: { uint32_t c = (uint32_t)v % (2 * size); return (int)(c >= size ? 2 * size - 1 - c : c); }
    case 4: return v < 0 ? (int)((uint32_t)v % size) : v >= (int32_t)size ? (int)size - 1 : v;
    default: return (int)((uint32_t)v % size);
    }
}
static void sample(const TexUnit *t, float u, float v, uint8_t out[4]) {
    int32_t s = (int32_t)(u * (float)t->w), tt = (int32_t)(v * (float)t->h);
    int border = (t->wrap_s == 1 && (s < 0 || s >= (int32_t)t->w)) || (t->wrap_t == 1 && (tt < 0 || tt >= (int32_t)t->h)) ||
                 (t->wrap_s == 5 && s >= (int32_t)t->w) || (t->wrap_t == 5 && tt >= (int32_t)t->h);
    if (border) { out[0] = (uint8_t)t->border; out[1] = (uint8_t)(t->border >> 8); out[2] = (uint8_t)(t->border >> 16); out[3] = (uint8_t)(t->border >> 24); return; }
    uint32_t x = (uint32_t)wrap(s, t->w, t->wrap_s), y = t->h - 1 - (uint32_t)wrap(tt, t->h, t->wrap_t);
    texel(t, x, y, out);
}
static void setup_tex(TexUnit *t, int unit) {
    static const uint32_t base_reg[3] = { 0x081, 0x091, 0x099 }, fmt_reg[3] = { 0x08e, 0x096, 0x09e };
    uint32_t b = base_reg[unit];
    t->on = (reg[0x080] >> unit) & 1;
    if (!t->on) return;
    t->h = reg[b + 1] & 0x7ff; t->w = (reg[b + 1] >> 16) & 0x7ff; t->fmt = reg[fmt_reg[unit]] & 15;
    t->wrap_t = (reg[b + 2] >> 8) & 7; t->wrap_s = (reg[b + 2] >> 12) & 7; t->border = reg[b];
    t->param = reg[b + 2]; t->lod = reg[b + 3];   /* filters (bits 1, 2, 24) and mip levels: used by the Vulkan backend */
    uint32_t type = unit == 0 ? (reg[b + 2] >> 28) & 7 : 0;
    if (type != 0 && type != 1 && type != 3 && type != 4 && !warned_tex[type]) { warned_tex[type] = 1; LOG(1, "pica: texture type %u (cube/shadow) is sampled as 2D", type); }
    t->base = (t->w && t->h && tex_bits[t->fmt]) ? phys((reg[b + 4] & 0x0fffffff) << 3, t->w * t->h * tex_bits[t->fmt] / 8) : 0;
    if (!t->base) t->on = 0;
    t->cube = 0;
    if (t->on && (type == 1 || type == 4)) {           /* cube map (unit 0 only): +X at the main address, the other faces at
                                                          0x86-0x8A (low 22 bits; bits 22-27 shared with the main address) */
        uint32_t main = reg[b + 4] & 0x0fffffff, size = t->w * t->h * tex_bits[t->fmt] / 8;
        t->cube = 1; t->face[0] = t->base;
        for (int f = 1; f < 6; f++) {
            t->face[f] = phys(((main & ~0x3fffffu) | (reg[0x086 + f - 1] & 0x3fffffu)) << 3, size);
            if (!t->face[f]) t->cube = 0, t->on = 0;
        }
    }

}

/* ---- texture combiner ---- */
static const uint32_t tev_reg[6] = { 0x0c0, 0x0c8, 0x0d0, 0x0d8, 0x0f0, 0x0f8 };
static void tev_operand(uint32_t m, const uint8_t s[4], uint8_t o[3]) {
    switch (m) {
    case 0: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; return;
    case 1: o[0] = 255 - s[0]; o[1] = 255 - s[1]; o[2] = 255 - s[2]; return;
    case 2: o[0] = o[1] = o[2] = s[3]; return;
    case 3: o[0] = o[1] = o[2] = 255 - s[3]; return;
    case 4: o[0] = o[1] = o[2] = s[0]; return;
    case 5: o[0] = o[1] = o[2] = 255 - s[0]; return;
    case 8: o[0] = o[1] = o[2] = s[1]; return;
    case 9: o[0] = o[1] = o[2] = 255 - s[1]; return;
    case 12: o[0] = o[1] = o[2] = s[2]; return;
    case 13: o[0] = o[1] = o[2] = 255 - s[2]; return;
    default: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; return;
    }
}
static uint8_t tev_alpha_operand(uint32_t m, const uint8_t s[4]) {
    static const int ch[8] = { 3, 3, 0, 0, 1, 1, 2, 2 };
    uint8_t v = s[ch[m & 7]];
    return (m & 1) ? (uint8_t)(255 - v) : v;
}
static int tev_op(uint32_t op, int a, int b, int c) {
    switch (op) {
    case 0: return a;
    case 1: return a * b / 255;
    case 2: return a + b > 255 ? 255 : a + b;
    case 3: { int v = a + b - 128; return v < 0 ? 0 : v > 255 ? 255 : v; }
    case 4: return (a * c + b * (255 - c)) / 255;
    case 5: return a - b < 0 ? 0 : a - b;
    case 8: { int v = (a * b + 255 * c) / 255; return v > 255 ? 255 : v; }
    case 9: return (a + b > 255 ? 255 : a + b) * c / 255;
    default: return 0;
    }
}
/* Combiner stages decoded once per draw (setup_tev). A stage that only passes the previous result
 * through is skipped, apart from the combiner-buffer bookkeeping. */
typedef struct {
    uint8_t csrc[3], asrc[3], cmod[3], amod[3], cop, aop, cscale, ascale, upd_c, upd_a, pass;
    uint8_t k[4];
} TevStage;
static TevStage tevs[6];
static uint8_t tev_buf_init[4];
static void setup_tev(void) {
    uint32_t bc = reg[0x0fd], upd = reg[0x0e0];
    for (int c = 0; c < 4; c++) tev_buf_init[c] = (uint8_t)(bc >> (8 * c));
    for (int i = 0; i < 6; i++) {
        uint32_t b = tev_reg[i], src = reg[b], opd = reg[b + 1], ops = reg[b + 2], k = reg[b + 3], sc = reg[b + 4];
        TevStage *t = &tevs[i];
        for (int j = 0; j < 3; j++) {
            uint32_t cs = (src >> (4 * j)) & 15, as = (src >> (16 + 4 * j)) & 15;
            if (i == 0 && cs == 15) cs = (src >> 8) & 15;
            t->csrc[j] = (uint8_t)cs; t->asrc[j] = (uint8_t)as;
            t->cmod[j] = (uint8_t)((opd >> (4 * j)) & 15); t->amod[j] = (uint8_t)((opd >> (12 + 4 * j)) & 7);
        }
        t->cop = ops & 15; t->aop = (ops >> 16) & 15;
        t->cscale = (uint8_t)((sc & 3) < 3 ? 1 << (sc & 3) : 1); t->ascale = (uint8_t)(((sc >> 16) & 3) < 3 ? 1 << ((sc >> 16) & 3) : 1);
        for (int c = 0; c < 4; c++) t->k[c] = (uint8_t)(k >> (8 * c));
        t->upd_c = (upd >> (8 + i)) & 1; t->upd_a = (upd >> (12 + i)) & 1;
        t->pass = i > 0 && t->cop == 0 && t->aop == 0 && t->csrc[0] == 15 && t->asrc[0] == 15 && t->cmod[0] == 0 && t->amod[0] == 0
                  && t->cscale == 1 && t->ascale == 1;
    }
}
static void combine(const uint8_t vcol[4], const uint8_t tex[4][4], const uint8_t prim[4], const uint8_t sec[4], uint8_t out[4]) {
    uint8_t prev[4] = { 0, 0, 0, 0 }, buf[4] = { 0, 0, 0, 0 }, next[4];
    memcpy(next, tev_buf_init, 4);
    const uint8_t *srcs[16] = { vcol, prim, sec, tex[0], tex[1], tex[2], tex[3], prev, prev, prev, prev, prev, prev, buf, 0, prev };
    for (int i = 0; i < 6; i++) {
        const TevStage *t = &tevs[i];
        if (!t->pass) {
            srcs[14] = t->k;
            uint8_t c[3][3], a[3];
            for (int j = 0; j < 3; j++) {
                tev_operand(t->cmod[j], srcs[t->csrc[j]], c[j]);
                a[j] = tev_alpha_operand(t->amod[j], srcs[t->asrc[j]]);
            }
            uint8_t res[4];
            if (t->cop == 6 || t->cop == 7) {
                int d = 0;
                for (int ch = 0; ch < 3; ch++) d += ((2 * c[0][ch] - 255) * (2 * c[1][ch] - 255) + 128) / 256;
                d = d < 0 ? 0 : d > 255 ? 255 : d;
                res[0] = res[1] = res[2] = (uint8_t)d;
            } else for (int ch = 0; ch < 3; ch++) res[ch] = (uint8_t)tev_op(t->cop, c[0][ch], c[1][ch], c[2][ch]);
            res[3] = t->cop == 7 ? res[0] : (uint8_t)tev_op(t->aop, a[0], a[1], a[2]);
            if (t->cscale != 1) for (int ch = 0; ch < 3; ch++) { int v = res[ch] * t->cscale; res[ch] = (uint8_t)(v > 255 ? 255 : v); }
            if (t->ascale != 1) { int v = res[3] * t->ascale; res[3] = (uint8_t)(v > 255 ? 255 : v); }
            memcpy(prev, res, 4);
        }
        memcpy(buf, next, 4);                                          /* reference: the buffer lags one stage */
        if (t->upd_c) memcpy(next, prev, 3);
        if (t->upd_a) next[3] = prev[3];
    }
    memcpy(out, prev, 4);
}

/* ---- fragment lighting (behaviour per docs/pica_facts.md, "Fragment lighting") ---- */

static float f16f(uint32_t raw) {                   /* 1.5.10 float */
    raw &= 0xffff;
    uint32_t sign = raw >> 15, e = (raw >> 10) & 31, m = raw & 0x3ff;
    float v;
    if (e == 0) v = ldexpf((float)m / 1024.0f, -14);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
    return sign ? -v : v;
}
static float f20f(uint32_t raw) {                   /* 1.7.12 float */
    raw &= 0xfffff;
    uint32_t sign = raw >> 19, e = (raw >> 12) & 127, m = raw & 0xfff;
    float v;
    if (e == 0) v = ldexpf((float)m / 4096.0f, -62);
    else if (e == 127) v = m ? NAN : INFINITY;
    else v = ldexpf(1.0f + (float)m / 4096.0f, (int)e - 63);
    return sign ? -v : v;
}
static float lut_entry(uint32_t type, uint32_t idx, float delta) {
    uint32_t e = lut[type][idx & 255];
    float value = (float)(e & 0xfff) / 4095.0f, diff = (float)((e >> 12) & 0x7ff) / 2047.0f;
    if (e & (1u << 23)) diff = -diff;
    return value + diff * delta;
}
static int lut_supported(uint32_t id, uint32_t config) {    /* id: 0 D0, 1 D1, 2 SP, 3 FR, 4 RB, 5 RG, 6 RR */
    switch (id) {
    case 0: return config != 1;
    case 1: return config != 0 && config != 1 && config != 5;
    case 2: return config != 2 && config != 3;
    case 3: return config != 0 && config != 2 && config != 4;
    case 6: return config != 3;
    default: return config == 4 || config == 5 || config == 8;
    }
}
static const float lut_scales[8] = { 1, 2, 4, 8, 0, 0, 0.25f, 0.5f };
static void norm3(float v[3]) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void qrot(const float q[4], const float v[3], float o[3]) {
    float t[3], u[3];
    cross3(q, v, t);
    for (int i = 0; i < 3; i++) t[i] += v[i] * q[3];
    cross3(q, t, u);
    for (int i = 0; i < 3; i++) o[i] = v[i] + 2.0f * u[i];
}
static float light_color(uint32_t r, int ch) { return (float)((r >> (ch == 0 ? 20 : ch == 1 ? 10 : 0)) & 0x3ff) / 255.0f; }

/* Light parameters decoded once per draw (setup_lighting). */
typedef struct { uint32_t id, cfg; float pos[3], spot[3], spec0[3], spec1[3], diff[3], amb[3], dbias, dscale; } LightP;
static LightP lights[8];
static uint32_t nlights;
static float global_amb[3];
static void setup_lighting(void) {
    nlights = (reg[0x1c2] & 7) + 1;
    for (uint32_t k = 0; k < nlights; k++) {
        LightP *l = &lights[k];
        uint32_t id = (reg[0x1d9] >> (4 * k)) & 7, b = 0x140 + 0x10 * id;
        l->id = id; l->cfg = reg[b + 9];
        l->pos[0] = f16f(reg[b + 4]); l->pos[1] = f16f(reg[b + 4] >> 16); l->pos[2] = f16f(reg[b + 5]);
        l->spot[0] = (float)((int32_t)(reg[b + 6] << 19) >> 19) / 2047.0f;
        l->spot[1] = (float)((int32_t)((reg[b + 6] >> 16) << 19) >> 19) / 2047.0f;
        l->spot[2] = (float)((int32_t)(reg[b + 7] << 19) >> 19) / 2047.0f;
        for (int ch = 0; ch < 3; ch++) {
            l->spec0[ch] = light_color(reg[b + 0], ch); l->spec1[ch] = light_color(reg[b + 1], ch);
            l->diff[ch] = light_color(reg[b + 2], ch); l->amb[ch] = light_color(reg[b + 3], ch);
        }
        l->dbias = f20f(reg[b + 0xa]); l->dscale = f20f(reg[b + 0xb]);
    }
    for (int ch = 0; ch < 3; ch++) global_amb[ch] = light_color(reg[0x1c0], ch);
}

/* Computes the primary (diffuse) and secondary (specular) fragment colours. */
static void lighting(const float quat_in[4], const float view[3], const uint8_t tex[4][4], uint8_t prim[4], uint8_t sec[4]) {
    uint32_t c0 = reg[0x1c3], c1 = reg[0x1c4], config = (c0 >> 4) & 15;
    uint32_t absin = reg[0x1d0], insel = reg[0x1d1], scl = reg[0x1d2];
    float q[4] = { quat_in[0], quat_in[1], quat_in[2], quat_in[3] };
    float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (ql > 0) for (int i = 0; i < 4; i++) q[i] /= ql;
    float shadow[4] = { 1, 1, 1, 1 };
    if (c0 & 1) {
        const uint8_t *t = tex[(c0 >> 24) & 3];
        for (int i = 0; i < 4; i++) { float v = (float)t[i] / 255.0f; shadow[i] = (c0 >> 18) & 1 ? 1.0f - v : v; }
    }
    float ns[3] = { 0, 0, 1 }, ts[3] = { 1, 0, 0 };
    uint32_t bump = (c0 >> 28) & 3;
    if (bump == 1 || bump == 2) {
        const uint8_t *t = tex[(c0 >> 22) & 3];
        float p[3] = { (float)t[0] / 127.5f - 1.0f, (float)t[1] / 127.5f - 1.0f, (float)t[2] / 127.5f - 1.0f };
        if (bump == 1) {
            if (!((c0 >> 30) & 1)) { float z = 1.0f - (p[0] * p[0] + p[1] * p[1]); p[2] = sqrtf(z > 0 ? z : 0); }
            memcpy(ns, p, sizeof ns);
        } else memcpy(ts, p, sizeof ts);
    } else if (bump == 3) { memset(ns, 0, sizeof ns); memset(ts, 0, sizeof ts); }
    float n[3], tg[3];
    qrot(q, ns, n); qrot(q, ts, tg);
    float V[3] = { view[0], view[1], view[2] };
    norm3(V);
    float dsum[4] = { 0, 0, 0, 1 }, ssum[4] = { 0, 0, 0, 1 };
    uint32_t nl = nlights;
    for (uint32_t k = 0; k < nl; k++) {
        const LightP *lp = &lights[k];
        uint32_t id = lp->id, cfg = lp->cfg;
        int two_sided = (cfg >> 1) & 1;
        float L[3] = { lp->pos[0], lp->pos[1], lp->pos[2] };
        if (!(cfg & 1)) for (int i = 0; i < 3; i++) L[i] += view[i];
        float len = sqrtf(dot3(L, L));
        norm3(L);
        float H[3] = { V[0] + L[0], V[1] + L[1], V[2] + L[2] };
        float hlen2 = dot3(H, H), Hn[3] = { H[0], H[1], H[2] };
        norm3(Hn);
        const float *spot = lp->spot;
        float in_val[6];
        in_val[0] = dot3(n, Hn); in_val[1] = dot3(V, Hn); in_val[2] = dot3(n, V); in_val[3] = dot3(L, n); in_val[4] = dot3(L, spot);
        { float nh = dot3(n, Hn), p[3] = { Hn[0] - n[0] * nh, Hn[1] - n[1] * nh, Hn[2] - n[2] * nh }; in_val[5] = dot3(p, tg); }
        /* sample one of the 7 material LUTs (lut_id 0..6) through its input selector, range and scale */
#define SAMPLE(lut_id, type) ({ \
            uint32_t sh_ = (lut_id) * 4; float x_ = in_val[((insel >> sh_) & 7) % 6]; float r_; \
            if (!((absin >> (sh_ + 1)) & 1)) { x_ = two_sided ? fabsf(x_) : (x_ > 0 ? x_ : 0); \
                int i_ = (int)floorf(x_ * 256.0f); i_ = i_ < 0 ? 0 : i_ > 255 ? 255 : i_; r_ = lut_entry((type), (uint32_t)i_, x_ * 256.0f - (float)i_); } \
            else { int i_ = (int)floorf(x_ * 128.0f); i_ = i_ < -128 ? -128 : i_ > 127 ? 127 : i_; \
                r_ = lut_entry((type), (uint32_t)(uint8_t)(int8_t)i_, x_ * 128.0f - (float)i_); } \
            r_ * lut_scales[(scl >> sh_) & 7]; })
        float spot_att = (!((c1 >> (8 + id)) & 1) && lut_supported(2, config)) ? SAMPLE(2, 8 + id) : 1.0f;
        float d0 = (!((c1 >> 16) & 1) && lut_supported(0, config)) ? SAMPLE(0, 0) : 1.0f;
        float refl[3];
        refl[0] = (!((c1 >> 20) & 1) && lut_supported(6, config)) ? SAMPLE(6, 6) : 1.0f;
        refl[1] = (!((c1 >> 21) & 1) && lut_supported(5, config)) ? SAMPLE(5, 5) : refl[0];
        refl[2] = (!((c1 >> 22) & 1) && lut_supported(4, config)) ? SAMPLE(4, 4) : refl[0];
        float d1 = (!((c1 >> 17) & 1) && lut_supported(1, config)) ? SAMPLE(1, 1) : 1.0f;
        if (k == nl - 1 && !((c1 >> 19) & 1) && lut_supported(3, config)) {
            float fr = SAMPLE(3, 3);
            if ((c0 >> 2) & 1) dsum[3] = fr;
            if ((c0 >> 3) & 1) ssum[3] = fr;
        }
#undef SAMPLE
        float dist = 1.0f;
        if (!((c1 >> (24 + id)) & 1)) {
            float s = lp->dscale * len + lp->dbias;
            s = s < 0 ? 0 : s > 1 ? 1 : s;
            int i = (int)floorf(s * 256.0f); i = i < 0 ? 0 : i > 255 ? 255 : i;
            dist = lut_entry(16 + id, (uint32_t)i, s * 256.0f - (float)i);
        }
        float ndl = two_sided ? fabsf(in_val[3]) : (in_val[3] > 0 ? in_val[3] : 0);
        float clamp_hl = ((c0 >> 27) & 1) && ndl == 0 ? 0.0f : 1.0f;
        float geo = hlen2 == 0 ? 0.0f : (ndl / hlen2 < 1 ? ndl / hlen2 : 1.0f);
        int sp_on = (c0 >> 16) & 1 && !((c1 >> id) & 1), ss_on = (c0 >> 17) & 1 && !((c1 >> id) & 1);
        for (int ch = 0; ch < 3; ch++) {
            float s0 = d0 * lp->spec0[ch], s1 = d1 * refl[ch] * lp->spec1[ch];
            if ((cfg >> 2) & 1) s0 *= geo;
            if ((cfg >> 3) & 1) s1 *= geo;
            float shp = sp_on ? shadow[ch] : 1.0f, shs = ss_on ? shadow[ch] : 1.0f;
            dsum[ch] += (lp->diff[ch] * ndl * shp + lp->amb[ch]) * dist * spot_att;
            ssum[ch] += (s0 + s1) * clamp_hl * dist * spot_att * shs;
        }
    }
    if ((c0 >> 19) & 1) {
        if ((c0 >> 2) & 1) dsum[3] *= shadow[3];
        if ((c0 >> 3) & 1) ssum[3] *= shadow[3];
    }
    for (int ch = 0; ch < 3; ch++) dsum[ch] += global_amb[ch];
    for (int i = 0; i < 4; i++) {
        float d = dsum[i] < 0 ? 0 : dsum[i] > 1 ? 1 : dsum[i], s = ssum[i] < 0 ? 0 : ssum[i] > 1 ? 1 : ssum[i];
        prim[i] = (uint8_t)(d * 255.0f); sec[i] = (uint8_t)(s * 255.0f);
    }
}

/* ---- per-draw fragment state ---- */
static TexUnit tu[3];
static int flat_mode = -1;
static uint8_t flat[4];

static uint8_t blend_factor(uint32_t f, int ch, const uint8_t s[4], const uint8_t d[4], const uint8_t k[4]) {
    switch (f) {
    case 0: return 0; case 1: return 255;
    case 2: return s[ch]; case 3: return 255 - s[ch]; case 4: return d[ch]; case 5: return 255 - d[ch];
    case 6: return s[3]; case 7: return 255 - s[3]; case 8: return d[3]; case 9: return 255 - d[3];
    case 10: return k[ch]; case 11: return 255 - k[ch]; case 12: return k[3]; case 13: return 255 - k[3];
    case 14: return ch == 3 ? 255 : (s[3] < 255 - d[3] ? s[3] : 255 - d[3]);
    default: return 255;
    }
}
static uint8_t logic(uint32_t op, uint8_t s, uint8_t d) {
    switch (op) {
    case 0: return 0; case 1: return s & d; case 2: return s & ~d; case 3: return s; case 4: return 255;
    case 5: return ~s; case 6: return d; case 7: return ~d; case 8: return ~(s & d); case 9: return s | d;
    case 10: return ~(s | d); case 11: return s ^ d; case 12: return ~(s ^ d); case 13: return ~s & d;
    case 14: return s | ~d; default: return ~s | d;
    }
}
static void color_read(const uint8_t *p, uint32_t fmt, uint8_t c[4]) {
    uint32_t v = (uint32_t)(p[0] | p[1] << 8);
    switch (fmt) {
    case 0: c[0] = p[3]; c[1] = p[2]; c[2] = p[1]; c[3] = p[0]; return;
    case 1: c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; c[3] = 255; return;
    case 2: c[0] = x5(v >> 11); c[1] = x5((v >> 6) & 31); c[2] = x5((v >> 1) & 31); c[3] = (v & 1) ? 255 : 0; return;
    case 3: c[0] = x5(v >> 11); c[1] = (uint8_t)((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4)); c[2] = x5(v & 31); c[3] = 255; return;
    default: c[0] = x4(v >> 12); c[1] = x4((v >> 8) & 15); c[2] = x4((v >> 4) & 15); c[3] = x4(v & 15); return;
    }
}
static int stencil_op(uint32_t act, int old, int ref) {
    switch (act) {
    case 1: return 0; case 2: return ref; case 3: return old < 255 ? old + 1 : 255; case 4: return old > 0 ? old - 1 : 0;
    case 5: return ~old & 255; case 6: return (old + 1) & 255; case 7: return (old - 1) & 255; default: return old;
    }
}

/* Cube-map direction (u, v, w) -> face and face coordinates (behaviour per the Azahar software renderer). */
static int cube_coord(float u, float v, float w, float *ou, float *ov) {
    float au = fabsf(u), av = fabsf(v), aw = fabsf(w), x, y, z; int face;
    if (au > av && au > aw) { if (u > 0) { face = 0; y = -v; } else { face = 1; y = v; } x = -w; z = u; }
    else if (av > aw) { if (v > 0) { face = 2; x = u; } else { face = 3; x = -u; } y = w; z = v; }
    else { if (w > 0) { face = 4; y = -v; } else { face = 5; y = v; } x = u; z = w; }
    *ou = x / z * 0.5f + 0.5f; *ov = y / z * 0.5f + 0.5f;
    return face;
}

/* Interpolated fragment colour; returns 0 if the fragment is discarded by the alpha test. */
/* ---- procedural texture (texture unit 3), as Azahar's sw_proctex.cpp ---- */
static float half_f(uint32_t h) {                    /* float16 register fields */
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 0x3ff;
    float f = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -f : f;
}
static float pt_lookup(const uint32_t *t, float coord) {      /* value entry: 0.12 value, signed 0.12 delta */
    coord *= 128.0f;
    int i = (int)coord; if (i > 127) i = 127; if (i < 0) i = 0;
    float frac = coord - (float)i;
    return (float)(t[i] & 0xfff) / 4095.0f + frac * (float)((int32_t)(t[i] << 8) >> 20) / 4095.0f;
}
static uint32_t pt_rand1(uint32_t v) {
    static const uint32_t tb[16] = { 0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11 };
    return (((v % 9 + 2) * 3) & 0xF) ^ tb[(v / 9) & 0xF];
}
static float pt_rand2(uint32_t x, uint32_t y) {
    static const uint32_t tb[16] = { 10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14 };
    uint32_t u2 = pt_rand1(x), v2 = pt_rand1(y);
    v2 += (u2 & 3) == 1 ? 4 : 0; v2 ^= (u2 & 1) * 6; v2 += 10 + u2; v2 &= 0xF; v2 ^= tb[u2];
    return -1.0f + (float)v2 * 2.0f / 15.0f;
}
static float pt_shift(float v, uint32_t mode, uint32_t clamp) {
    float off = clamp == 3 ? 1.0f : 0.5f;
    if (mode == 1) return off * (float)(((int)v / 2) % 2);
    if (mode == 2) return off * (float)((((int)v + 1) / 2) % 2);
    return 0.0f;
}
static float pt_clamp(float c, uint32_t mode) {
    switch (mode) {
    case 0: return c > 1.0f ? 0.0f : c;
    case 2: return c - floorf(c);
    case 3: { int i = (int)c; float f = c - (float)i; return i % 2 == 0 ? f : 1.0f - f; }
    case 4: return c <= 0.5f ? 0.0f : 1.0f;
    default: return c < 1.0f ? c : 1.0f;
    }
}
static float pt_combine(float u, float v, uint32_t k, const uint32_t *map) {
    float f;
    switch (k) {
    case 0: f = u; break; case 1: f = u * u; break; case 2: f = v; break; case 3: f = v * v; break;
    case 4: f = (u + v) * 0.5f; break; case 5: f = (u * u + v * v) * 0.5f; break;
    case 6: f = fminf(sqrtf(u * u + v * v), 1.0f); break; case 7: f = fminf(u, v); break; case 8: f = fmaxf(u, v); break;
    case 9: f = fminf(((u + v) * 0.5f + sqrtf(u * u + v * v)) * 0.5f, 1.0f); break;
    default: f = 0.0f;
    }
    return pt_lookup(map, f);
}
static void proctex(float u, float v, uint8_t out[4]) {
    uint32_t c = reg[0x0a8];
    u = fabsf(u); v = fabsf(v);
    float us = pt_shift(v, (c >> 16) & 3, c & 7), vs_ = pt_shift(u, (c >> 18) & 3, (c >> 3) & 7);
    if ((c >> 15) & 1) {                                      /* noise */
        float fu = half_f(reg[0x0ab] & 0xffff), fv = half_f(reg[0x0ab] >> 16);
        float pu = half_f(reg[0x0a9] >> 16), pv = half_f(reg[0x0aa] >> 16);
        float x = 9.0f * fu * fabsf(u + pu), y = 9.0f * fv * fabsf(v + pv);
        int xi = (int)x, yi = (int)y; float xf = x - (float)xi, yf = y - (float)yi;
        float g0 = pt_rand2((uint32_t)xi, (uint32_t)yi) * (xf + yf), g1 = pt_rand2((uint32_t)xi + 1, (uint32_t)yi) * (xf + yf - 1);
        float g2 = pt_rand2((uint32_t)xi, (uint32_t)yi + 1) * (xf + yf - 1), g3 = pt_rand2((uint32_t)xi + 1, (uint32_t)yi + 1) * (xf + yf - 2);
        float xn = pt_lookup(pt_lut[0], xf), yn = pt_lookup(pt_lut[0], yf);
        float n = (g0 * (1 - xn) + g1 * xn) * (1 - yn) + (g2 * (1 - xn) + g3 * xn) * yn;
        u = fabsf(u + n * (float)(int16_t)(reg[0x0a9] & 0xffff) / 4095.0f);
        v = fabsf(v + n * (float)(int16_t)(reg[0x0aa] & 0xffff) / 4095.0f);
    }
    u = pt_clamp(u + us, c & 7); v = pt_clamp(v + vs_, (c >> 3) & 7);
    float lc = pt_combine(u, v, (c >> 6) & 15, pt_lut[2]);
    uint32_t off = reg[0x0ad] & 0xff, width = (reg[0x0ac] >> 11) & 0xff, filt = reg[0x0ac] & 7;
    float idx = (float)off + lc * (float)(width - 1);
    if (filt == 1 || filt == 3 || filt == 5) {                /* linear between colour entries */
        int ii = (int)idx; if (ii < 0) ii = 0; if (ii > 255) ii = 255;
        float fr = idx - (float)ii;
        uint32_t cv = pt_lut[4][ii], cd = pt_lut[5][ii];
        for (int k = 0; k < 4; k++) {
            float x = (float)((cv >> (8 * k)) & 255) + fr * (float)((int8_t)(cd >> (8 * k)) * 2);
            out[k] = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
        }
    } else {
        int ii = (int)lrintf(idx); if (ii < 0) ii = 0; if (ii > 255) ii = 255;
        for (int k = 0; k < 4; k++) out[k] = (uint8_t)(pt_lut[4][ii] >> (8 * k));
    }
    if ((c >> 14) & 1) {                                      /* separate alpha: straight from its own combiner */
        float a = pt_combine(u, v, (c >> 10) & 15, pt_lut[3]);
        out[3] = (uint8_t)(a * 255.0f);
    }
}

static int fragment(const Vtx *v0, const Vtx *v1, const Vtx *v2, float b0, float b1, float b2, uint8_t out[4]) {
    if (flat_mode) { memcpy(out, flat, 4); return 1; }
    float iw = 1.0f / (b0 * v0->invw + b1 * v1->invw + b2 * v2->invw);
#define I(k) ((b0 * v0->a[k] + b1 * v1->a[k] + b2 * v2->a[k]) * iw)
    uint8_t vcol[4];
    for (int c = 0; c < 4; c++) { float f = I(S_COL + c) * 255.0f; vcol[c] = (uint8_t)(f < 0 ? 0 : f > 255 ? 255 : lrintf(f)); }
    uint8_t tex[4][4] = { { 0 } };
    if (tu[0].on) {
        float u = I(S_TC0), v = I(S_TC0 + 1);
        if (tu[0].cube) { float cu, cv; TexUnit t = tu[0]; t.base = t.face[cube_coord(u, v, I(S_TC0W), &cu, &cv)]; sample(&t, cu, cv, tex[0]); }
        else {
            if (((reg[0x083] >> 28) & 7) == 3) { float w = I(S_TC0W); u /= w; v /= w; }
            sample(&tu[0], u, v, tex[0]);
        }
    }
    if (tu[1].on) sample(&tu[1], I(S_TC1), I(S_TC1 + 1), tex[1]);
    if (tu[2].on) {
        int c1 = (reg[0x080] >> 13) & 1;
        sample(&tu[2], c1 ? I(S_TC1) : I(S_TC2), c1 ? I(S_TC1 + 1) : I(S_TC2 + 1), tex[2]);
    }
    if ((reg[0x080] >> 10) & 1) {                            /* procedural texture from the coordinates of 0x080 bits 8-9 */
        uint32_t cs = (reg[0x080] >> 8) & 3; int k = cs == 0 ? S_TC0 : cs == 1 ? S_TC1 : S_TC2;
        proctex(I(k), I(k + 1), tex[3]);
    }
    uint8_t prim[4] = { 0, 0, 0, 0 }, sec[4] = { 0, 0, 0, 0 };
    if (!(reg[0x1c6] & 1)) {                         /* lighting enabled (the reference gates on 0x1C6 only) */
        float q[4] = { I(S_QUAT), I(S_QUAT + 1), I(S_QUAT + 2), I(S_QUAT + 3) }, view[3] = { I(S_VIEW), I(S_VIEW + 1), I(S_VIEW + 2) };
        lighting(q, view, tex, prim, sec);
    }
#undef I
    combine(vcol, tex, prim, sec, out);
    uint32_t at = reg[0x104];
    if ((at & 1) && !compare((at >> 4) & 7, out[3], (at >> 8) & 0xff)) return 0;
    return 1;
}

/* Fog (behaviour per the Azahar software renderer): after the alpha test, the clamped fragment depth
 * (flipped when 0x0E0 bit 16 is set) indexes a 128-entry LUT of 11-bit value + signed 13-bit difference,
 * and the combiner RGB is mixed towards the fog colour (0x0E1). Gas mode (7) is not implemented. */
static void apply_fog(uint8_t c[4], float depth) {
    if ((reg[0x0e0] & 7) != 5) return;
    float idx = (reg[0x0e0] >> 16) & 1 ? (1.0f - depth) * 128.0f : depth * 128.0f;
    float fi = floorf(idx); fi = fi < 0 ? 0 : fi > 127 ? 127 : fi;
    uint32_t e = fog_lut[(uint32_t)fi];
    float f = (float)((e >> 13) & 0x7ff) / 2047.0f + (float)((int32_t)(e << 19) >> 19) / 2047.0f * (idx - fi);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    uint32_t fc = reg[0x0e1];
    for (int i = 0; i < 3; i++) c[i] = (uint8_t)(f * (float)c[i] + (1.0f - f) * (float)((fc >> (8 * i)) & 0xff));
}

/* ---- rasteriser ---- */
static int64_t area(const Vtx *a, const Vtx *b, int32_t cx, int32_t cy) {
    return (int64_t)(b->x - a->x) * (cy - a->y) - (int64_t)(b->y - a->y) * (cx - a->x);
}
static int right_or_flat_bottom(const Vtx *v, const Vtx *l1, const Vtx *l2) {
    if (l1->y == l2->y) return v->y < l1->y;
    return v->x < l1->x + (l2->x - l1->x) * (v->y - l1->y) / (l2->y - l1->y);
}
/* Triangles of the current draw, culled and in counter-clockwise order, rasterised together at the end
 * of the draw (in parallel horizontal bands; each band keeps the triangle order). */
typedef struct { Vtx v[3]; } Tri;
static Tri *tris; static int ntris, tris_cap;
typedef struct { uint64_t pixels, cov, alpha, depth, stencil; } FragStats;

static void queue_tri(const Vtx *v0, const Vtx *v1, const Vtx *v2) {
    int64_t ar = area(v0, v1, v2->x, v2->y);
    uint32_t cull = reg[0x040] & 3;
    if (cull == 2 && ar <= 0) { n_culled++; return; }
    if (cull == 1) { if (ar >= 0) { n_culled++; return; } const Vtx *t = v1; v1 = v2; v2 = t; }
    else if (ar < 0) { const Vtx *t = v1; v1 = v2; v2 = t; }
    if (ar == 0) return;
    n_tris++;
    if (ntris == tris_cap) { tris_cap = tris_cap ? tris_cap * 2 : 1024; tris = realloc(tris, (size_t)tris_cap * sizeof *tris); if (!tris) fatal("out of memory for triangles"); }
    tris[ntris].v[0] = *v0; tris[ntris].v[1] = *v1; tris[ntris].v[2] = *v2; ntris++;
}

/* Rasterise one queued triangle, limited to pixel rows [row_lo, row_hi). */
static void raster_band(const Fb *fb, const Tri *t, int32_t row_lo, int32_t row_hi, FragStats *fs) {
    const Vtx *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
    int32_t minx = v0->x, maxx = v0->x, miny = v0->y, maxy = v0->y;
    const Vtx *vv[3] = { v0, v1, v2 };
    for (int i = 1; i < 3; i++) { if (vv[i]->x < minx) minx = vv[i]->x; if (vv[i]->x > maxx) maxx = vv[i]->x; if (vv[i]->y < miny) miny = vv[i]->y; if (vv[i]->y > maxy) maxy = vv[i]->y; }
    minx &= ~15; miny &= ~15; maxx = (maxx + 15) & ~15; maxy = (maxy + 15) & ~15;
    uint32_t sc = reg[0x065] & 3, sx1 = reg[0x066] & 0x3ff, sy1 = (reg[0x066] >> 16) & 0x3ff, sx2 = reg[0x067] & 0x3ff, sy2 = (reg[0x067] >> 16) & 0x3ff;
    if (sc == 3) {
        if (minx < (int32_t)(sx1 << 4)) minx = (int32_t)(sx1 << 4);
        if (miny < (int32_t)(sy1 << 4)) miny = (int32_t)(sy1 << 4);
        if (maxx > (int32_t)((sx2 + 1) << 4)) maxx = (int32_t)((sx2 + 1) << 4);
        if (maxy > (int32_t)((sy2 + 1) << 4)) maxy = (int32_t)((sy2 + 1) << 4);
    }
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > (int32_t)(fb->width << 4)) maxx = (int32_t)(fb->width << 4);
    if (maxy > (int32_t)((fb->hreg + 1) << 4)) maxy = (int32_t)((fb->hreg + 1) << 4);
    if (miny < row_lo << 4) miny = row_lo << 4;
    if (maxy > row_hi << 4) maxy = row_hi << 4;
    if (miny >= maxy || minx >= maxx) return;
    int64_t b0 = right_or_flat_bottom(v0, v1, v2) ? -1 : 0, b1 = right_or_flat_bottom(v1, v2, v0) ? -1 : 0, b2 = right_or_flat_bottom(v2, v0, v1) ? -1 : 0;
    float zscale = f24(reg[0x04d]), zoff = f24(reg[0x04e]);
    int zbuf = reg[0x06d] & 1;
    uint32_t om = reg[0x107], dtest = om & 1, dfunc = (om >> 4) & 7, dwrite = (om >> 12) & 1;
    uint32_t allow_c = reg[0x113] & 15, allow_d = reg[0x115] & 3;
    int early_ds = !(reg[0x104] & 1);
    uint32_t dbits = fb->dfmt == 0 ? 16 : 24, dmax = (1u << dbits) - 1;
    for (int32_t y = miny + 8; y < maxy; y += 16)
        for (int32_t x = minx + 8; x < maxx; x += 16) {
            int64_t w0 = area(v1, v2, x, y) + b0, w1 = area(v2, v0, x, y) + b1, w2 = area(v0, v1, x, y) + b2;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            uint32_t px = (uint32_t)x >> 4, py = (uint32_t)y >> 4;
            if (sc == 1 && px >= sx1 && px <= sx2 && py >= sy1 && py <= sy2) continue;
            float sum = (float)(w0 + w1 + w2);
            float z = ((float)w0 * v0->z + (float)w1 * v1->z + (float)w2 * v2->z) / sum;
            float depth = z * zscale + zoff;
            if (!zbuf) depth *= sum / ((float)w0 * v0->invw + (float)w1 * v1->invw + (float)w2 * v2->invw);
            depth = depth < 0 ? 0 : depth > 1 ? 1 : depth;
            uint8_t col[4];
            fs->cov++;
            /* Early depth/stencil: without the alpha test the colour cannot change the depth or stencil outcome,
             * so hidden pixels skip texturing, combiners and lighting. The result is identical. */
            if (!early_ds && !fragment(v0, v1, v2, (float)w0, (float)w1, (float)w2, col)) { fs->alpha++; continue; }
            uint32_t zi = (uint32_t)(depth * (float)dmax);
            uint8_t *dp = fb->depth ? fb->depth + fb_off(fb, px, py, fb->dbpp) : 0;
            uint32_t st = reg[0x105], sa = reg[0x106];
            int stencil = (st & 1) && fb->dfmt == 3 && dp;
            int sold = stencil ? dp[3] : 0, sref = (int)((st >> 16) & 0xff), sin = (int)((st >> 24) & 0xff), swm = (int)((st >> 8) & 0xff);
            if (stencil && !compare((st >> 4) & 7, (uint32_t)(sref & sin), (uint32_t)(sold & sin))) {
                if (allow_d) dp[3] = (uint8_t)((stencil_op(sa & 7, sold, sref) & swm) | (sold & ~swm));
                fs->stencil++;
                continue;
            }
            if (dtest) {
                uint32_t old = dp ? (fb->dfmt == 0 ? (uint32_t)(dp[0] | dp[1] << 8) : (uint32_t)(dp[0] | dp[1] << 8 | dp[2] << 16)) : 0;
                if (!compare(dfunc, zi, old)) {
                    fs->depth++;
                    if (stencil && allow_d) dp[3] = (uint8_t)((stencil_op((sa >> 4) & 7, sold, sref) & swm) | (sold & ~swm));
                    continue;
                }
            }
            if (dp && allow_d && dwrite) {
                dp[0] = (uint8_t)zi; dp[1] = (uint8_t)(zi >> 8);
                if (fb->dfmt != 0) dp[2] = (uint8_t)(zi >> 16);
            }
            if (stencil && allow_d) dp[3] = (uint8_t)((stencil_op((sa >> 8) & 7, sold, sref) & swm) | (sold & ~swm));
            if (!allow_c || !fb->color) continue;
            if (early_ds) fragment(v0, v1, v2, (float)w0, (float)w1, (float)w2, col);
            if (!flat_mode) apply_fog(col, depth);
            uint8_t *cp = fb->color + fb_off(fb, px, py, fb->cbpp), dst[4], res[4];
            color_read(cp, fb->cfmt, dst);
            uint32_t bl = reg[0x101], kk = reg[0x103];
            uint8_t k[4] = { (uint8_t)kk, (uint8_t)(kk >> 8), (uint8_t)(kk >> 16), (uint8_t)(kk >> 24) };
            if (flat_mode) memcpy(res, col, 4);
            else if (reg[0x100] & 0x100) {
                for (int ch = 0; ch < 4; ch++) {
                    uint32_t sf = ch < 3 ? (bl >> 16) & 15 : (bl >> 24) & 15, df = ch < 3 ? (bl >> 20) & 15 : (bl >> 28) & 15;
                    uint32_t eq = ch < 3 ? bl & 7 : (bl >> 8) & 7;
                    int s = col[ch] * blend_factor(sf, ch, col, dst, k), d = dst[ch] * blend_factor(df, ch, col, dst, k), r;
                    switch (eq) {
                    case 1: r = (s - d) / 255; break;
                    case 2: r = (d - s) / 255; break;
                    case 3: r = (s < d ? s : d) / 255; break;
                    case 4: r = (s > d ? s : d) / 255; break;
                    default: r = (s + d) / 255; break;
                    }
                    res[ch] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
                }
            } else for (int ch = 0; ch < 4; ch++) res[ch] = logic(reg[0x102] & 15, col[ch], dst[ch]);
            for (int ch = 0; ch < 4; ch++) if (!((om >> (8 + ch)) & 1)) res[ch] = dst[ch];
            color_write(cp, fb->cfmt, res);
            fs->pixels++;
        }
}

/* ---- clipping ---- */
static int clip_poly(Vtx *in, int n, Vtx *out, const float c[4], float bias_w) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vtx *cur = &in[i], *prev = &in[(i + n - 1) % n];
        float dc = cur->a[0] * c[0] + cur->a[1] * c[1] + cur->a[2] * c[2] + (cur->a[3] + bias_w) * c[3];
        float dp = prev->a[0] * c[0] + prev->a[1] * c[1] + prev->a[2] * c[2] + (prev->a[3] + bias_w) * c[3];
        int ic = dc >= -1e-8f, ip = dp >= -1e-8f;
        if (ic != ip && m < 12) {
            float f = dp / (dp - dc);
            Vtx *r = &out[m++];
            for (int k = 0; k < NSEM; k++) r->a[k] = cur->a[k] * f + prev->a[k] * (1.0f - f);
        }
        if (ic && m < 12) out[m++] = *cur;
    }
    return m;
}
static void to_screen(Vtx *v) {
    float hx = f24(reg[0x041]), hy = f24(reg[0x043]);
    int32_t cx = (int32_t)(reg[0x068] << 22) >> 22, cy = (int32_t)((reg[0x068] >> 16) << 22) >> 22;
    float iw = 1.0f / v->a[3];
    float sx = (v->a[0] * iw + 1.0f) * hx + (float)cx, sy = (v->a[1] * iw + 1.0f) * hy + (float)cy;
    v->z = v->a[2] * iw; v->invw = iw;
    for (int k = 4; k < NSEM; k++) v->a[k] *= iw;
    v->x = (int32_t)lrintf(sx * 16.0f); v->y = (int32_t)lrintf(sy * 16.0f);
}
static void triangle(const Fb *fb, const Vtx *a, const Vtx *b, const Vtx *c) {
    n_in_tris++;
    if (dump_left > 0) { dump_left--; fprintf(stderr, "  tri clip pos: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", a->a[0], a->a[1], a->a[2], a->a[3], b->a[0], b->a[1], b->a[2], b->a[3], c->a[0], c->a[1], c->a[2], c->a[3]); }
    static const float planes[6][4] = { { -1, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, -1, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, -1, 0 }, { 0, 0, 1, 1 } };
    static const float wplane[4] = { 0, 0, 0, 1 };
    Vtx buf1[12], buf2[12];
    buf1[0] = *a; buf1[1] = *b; buf1[2] = *c;
    for (int k = 1; k < 3; k++) {                    /* keep quaternions on the same hemisphere as vertex 0 */
        float d = 0; for (int i = 0; i < 4; i++) d += buf1[0].a[S_QUAT + i] * buf1[k].a[S_QUAT + i];
        if (d < 0) for (int i = 0; i < 4; i++) buf1[k].a[S_QUAT + i] = -buf1[k].a[S_QUAT + i];
    }
    int n = 3;
    Vtx *src = buf1, *dst = buf2;
    /* Trivial accept: when every vertex is inside every plane, clip_poly would copy its input unchanged, so skip it. */
    int inside = 1;
    for (int v = 0; v < 3 && inside; v++) {
        const float *q = buf1[v].a;
        for (int p = 0; p < 6; p++)
            if (q[0] * planes[p][0] + q[1] * planes[p][1] + q[2] * planes[p][2] + q[3] * planes[p][3] < -1e-8f) { inside = 0; break; }
        if (inside && (q[3] + 1e-5f) * wplane[3] < -1e-8f) inside = 0;
        if (inside && (reg[0x047] & 1) &&
            q[0] * f24(reg[0x048]) + q[1] * f24(reg[0x049]) + q[2] * f24(reg[0x04a]) + q[3] * f24(reg[0x04b]) < -1e-8f) inside = 0;
    }
    if (inside) { n_trivial++; goto accepted; }
    for (int p = 0; p < 7 && n >= 3; p++) {
        n = p < 6 ? clip_poly(src, n, dst, planes[p], 0.0f) : clip_poly(src, n, dst, wplane, 1e-5f);
        Vtx *t = src; src = dst; dst = t;
    }
    if (n < 3) { n_clipped++; return; }
    if (reg[0x047] & 1) {
        float cp[4] = { f24(reg[0x048]), f24(reg[0x049]), f24(reg[0x04a]), f24(reg[0x04b]) };
        n = clip_poly(src, n, dst, cp, 0.0f);
        Vtx *t = src; src = dst; dst = t;
        if (n < 3) return;
    }
accepted:
    for (int i = 0; i < n; i++) to_screen(&src[i]);
    for (int i = 0; i + 2 < n; i++) queue_tri(&src[0], &src[i + 1], &src[i + 2]);
    (void)fb;
}

/* ---- parallel rasterisation ---- */
static int nworkers = -1;
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_start = PTHREAD_COND_INITIALIZER, pool_done = PTHREAD_COND_INITIALIZER;
static uint64_t pool_gen; static int pool_pending;
static const Fb *job_fb; static int32_t job_band;
static FragStats job_stats[64];
static int job_kind;                                   /* 0 rasterise bands, 1 shade vertices, 2/3 geometry shader stages */
static void run_job(int b);
static uint32_t job_base, job_nv; static const uint32_t *job_vlist; static Vtx *job_vout;
static void shade_vertex(uint32_t base, uint32_t vertex, Vtx *out);
static uint32_t job_next;                               /* next vertex chunk to take (dynamic load balancing) */
static void run_vertices(int b, int n) {
    (void)b; (void)n;
    for (;;) {
        uint32_t i = __atomic_fetch_add(&job_next, 32, __ATOMIC_RELAXED);
        if (i >= job_nv) break;
        uint32_t e = i + 32 < job_nv ? i + 32 : job_nv;
        for (; i < e; i++) shade_vertex(job_base, job_vlist[i], &job_vout[job_vlist[i]]);
    }
}

static void run_band(int b) {
    FragStats st = { 0 };
    int32_t lo = b * job_band, hi = lo + job_band;
    for (int i = 0; i < ntris; i++) raster_band(job_fb, &tris[i], lo, hi, &st);
    job_stats[b] = st;
}
/* Workers spin briefly before sleeping: a frame has ~100+ small draws back to back, and waking sleeping threads for
 * each one cost the main thread more than the shading itself. */
#define POOL_SPIN 4000
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
static void *worker(void *arg) {
    int b = (int)(intptr_t)arg;
    uint64_t seen = 0;
    for (;;) {
        for (int spin = 0; __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) == seen; spin++) {
            if (spin < POOL_SPIN) { cpu_relax(); continue; }
            pthread_mutex_lock(&pool_mu);
            while (__atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) == seen) pthread_cond_wait(&pool_start, &pool_mu);
            pthread_mutex_unlock(&pool_mu);
        }
        seen = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE);
        run_job(b);
        if (__atomic_sub_fetch(&pool_pending, 1, __ATOMIC_ACQ_REL) == 0) {
            pthread_mutex_lock(&pool_mu); pthread_cond_signal(&pool_done); pthread_mutex_unlock(&pool_mu);
        }
    }
    return 0;
}
static void pool_init(void) {
    if (nworkers >= 0) return;
    const char *e = getenv("RB_THREADS");              /* RB_THREADS=n sets the thread count (1 = no threads) */
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    FILE *smt = fopen("/sys/devices/system/cpu/smt/active", "r");   /* one thread per physical core: hyperthread pairs
                                                                       sharing a core made shading slower, not faster */
    if (smt) { int on = 0; if (fscanf(smt, "%d", &on) == 1 && on && n >= 4) n /= 2; fclose(smt); }
    if (e) n = atol(e);
    nworkers = (int)(n < 1 ? 1 : n > 64 ? 64 : n);
    for (int i = 1; i < nworkers; i++) { pthread_t t; pthread_create(&t, 0, worker, (void *)(intptr_t)i); pthread_detach(t); }
}
static void pool_run(int kind) {                       /* run a job on all threads, including this one */
    job_kind = kind;
    __atomic_store_n(&pool_pending, nworkers - 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&pool_mu);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&pool_start);
    pthread_mutex_unlock(&pool_mu);
    run_job(0);
    uint64_t tw = pns();
    for (int spin = 0; __atomic_load_n(&pool_pending, __ATOMIC_ACQUIRE) && spin < POOL_SPIN * 4; spin++) cpu_relax();
    pthread_mutex_lock(&pool_mu);
    while (__atomic_load_n(&pool_pending, __ATOMIC_ACQUIRE)) pthread_cond_wait(&pool_done, &pool_mu);
    pthread_mutex_unlock(&pool_mu);
    t_pool_wait += pns() - tw;
}
static void shade_vertices(uint32_t base, const uint32_t *list, uint32_t n, Vtx *out) {
    pool_init();
    job_base = base; job_vlist = list; job_nv = n; job_vout = out; job_next = 0;
    if (nworkers == 1 || n < 48) run_vertices(0, 1);
    else pool_run(1);
}
static void flush_tris(const Fb *fb) {
    if (!ntris) return;
    pool_init();
    int32_t rows = (int32_t)fb->hreg + 1;
    int64_t work = 0;                                  /* rough pixel count, to keep tiny draws single-threaded */
    static int64_t min_work = -1; if (min_work < 0) { const char *e = getenv("RB_MIN_PAR"); min_work = e ? atoll(e) : 1024; }
    for (int i = 0; i < ntris && work < min_work; i++) {
        const Vtx *v = tris[i].v;
        int32_t w = (v[0].x > v[1].x ? v[0].x : v[1].x) - (v[0].x < v[1].x ? v[0].x : v[1].x);
        int32_t h = (v[0].y > v[2].y ? v[0].y : v[2].y) - (v[0].y < v[2].y ? v[0].y : v[2].y);
        work += ((int64_t)(w >> 4) + 1) * ((h >> 4) + 1) / 2;
    }
    job_fb = fb;
    if (nworkers == 1 || work < min_work) {
        job_band = rows; run_band(0);
        n_pixels += job_stats[0].pixels; frag_cov += job_stats[0].cov; frag_alpha += job_stats[0].alpha; frag_depth += job_stats[0].depth; frag_stencil += job_stats[0].stencil;
    } else {
        job_band = ((rows + nworkers - 1) / nworkers + 7) & ~7;
        pool_run(0);
        for (int b = 0; b < nworkers; b++) {
            n_pixels += job_stats[b].pixels; frag_cov += job_stats[b].cov; frag_alpha += job_stats[b].alpha;
            frag_depth += job_stats[b].depth; frag_stencil += job_stats[b].stencil;
        }
    }
    ntris = 0;
}

/* ---- Vulkan backend hand-off (vk.c): the GPU rasterises the triangles queued for this draw ---- */
/* Mip level l of a texture follows level l-1 in memory, each a quarter of the size; levels stop at 8x8 tiles. */
static uint32_t tex_levels(const TexUnit *t) {
    uint32_t n = ((t->lod >> 16) & 15) + 1, l = 1;
    while (l < n && (t->w >> l) >= 8 && (t->h >> l) >= 8) l++;
    return l;
}
static uint32_t tex_bytes(const TexUnit *t, uint32_t levels) {
    uint32_t n = 0;
    for (uint32_t l = 0; l < levels; l++) n += (t->w >> l) * (t->h >> l) * tex_bits[t->fmt] / 8;
    return n;
}
static void vk_decode(int unit, int face, int level, uint8_t *rgba) {
    TexUnit t = tu[unit];
    if (t.cube) t.base = t.face[face];
    t.base += tex_bytes(&tu[unit], (uint32_t)level); t.w >>= level; t.h >>= level;
    for (uint32_t y = 0; y < t.h; y++)
        for (uint32_t x = 0; x < t.w; x++) texel(&t, x, y, rgba + 4 * (y * t.w + x));
}
static void vk_submit(const Fb *fb, uint32_t caddr, uint32_t daddr) {
    static VkrDraw d;
    d.caddr = fb->color ? (uint32_t)(uintptr_t)fb->color : 0; d.daddr = fb->depth ? (uint32_t)(uintptr_t)fb->depth : 0;
    (void)caddr; (void)daddr;
    d.width = fb->width; d.height = fb->hreg + 1; d.cfmt = fb->cfmt; d.dfmt = fb->dfmt;
    d.regs = reg; d.lut = &lut[0][0]; d.fog_lut = fog_lut; d.pt_lut = &pt_lut[0][0]; d.lut_version = lut_version;
    for (uint32_t k = 0; k < 8; k++) {
        const LightP *l = &lights[k];
        float (*L)[4] = &d.lights[k * 6];
        for (int c = 0; c < 3; c++) { L[0][c] = l->pos[c]; L[1][c] = l->spot[c]; L[2][c] = l->spec0[c]; L[3][c] = l->spec1[c]; L[4][c] = l->diff[c]; L[5][c] = l->amb[c]; }
        L[0][3] = l->dbias; L[1][3] = l->dscale; memcpy(&L[2][3], &l->id, 4); memcpy(&L[3][3], &l->cfg, 4); L[4][3] = L[5][3] = 0;
    }
    for (int c = 0; c < 3; c++) d.gamb[c] = global_amb[c];
    d.zscale = f24(reg[0x04d]); d.zoff = f24(reg[0x04e]);
    d.flat = (uint32_t)flat_mode; d.flat_color = (uint32_t)(flat[0] | flat[1] << 8 | flat[2] << 16 | 0xffu << 24); d.nlights = nlights;
    for (int u = 0; u < 3; u++) {
        const TexUnit *t = &tu[u];
        d.tex[u].on = (uint32_t)t->on;
        if (!t->on) continue;
        d.tex[u].addr = (uint32_t)(uintptr_t)t->base; d.tex[u].w = t->w; d.tex[u].h = t->h; d.tex[u].fmt = t->fmt;
        uint32_t lv = tex_levels(t);
        if (!mem_valid(d.tex[u].addr, tex_bytes(t, lv))) lv = 1;
        d.tex[u].levels = lv; d.tex[u].size = tex_bytes(t, lv); d.tex[u].wrap = t->wrap_s | t->wrap_t << 8; d.tex[u].border = t->border;
        d.tex[u].param = t->param; d.tex[u].lod = t->lod; d.tex[u].faces = t->cube ? 6 : 1;
        for (int f = 0; f < 6; f++) d.tex[u].face_addr[f] = t->cube ? (uint32_t)(uintptr_t)t->face[f] : d.tex[u].addr;
        if (t->cube) for (int f = 0; f < 6 && lv > 1; f++) if (!mem_valid(d.tex[u].face_addr[f], tex_bytes(t, lv))) { lv = 1; d.tex[u].levels = 1; d.tex[u].size = tex_bytes(t, 1); }
    }
    vkr_draw(&d, tris, ntris, vk_decode);
}

/* Feature audit: how often draws use PICA features that are missing or approximated here, so it is clear
 * which ones a game actually needs. Printed by pica_report(); first use is logged with its frame. */
enum { A_PROCTEX, A_TEX_CUBE, A_TEX_SHADOW, A_TEX_SHADOWCUBE, A_GAS_FOG, A_FRAG_GAS, A_FRAG_SHADOW, A_MINMAX_FACTORS,
       A_LOGIC_OP, A_WRAP_ODD, A_LOD_BIAS, A_TARGET_16BIT_BLEND, A_COUNT };
static const char *audit_name[A_COUNT] = { "procedural texture (unit 3)", "cube-map texture", "shadow texture", "shadow cube texture",
    "gas fog mode", "gas fragment mode", "shadow-map rendering mode", "blend min/max with non-one factors", "logic op other than copy",
    "wrap modes 4-7", "texture LOD bias", "blending into a 16-bit colour target" };
static uint64_t audit[A_COUNT];
static void audit_hit(int k) { if (!audit[k]++) LOG(1, "pica: audit: first draw using %s at frame %llu", audit_name[k], (unsigned long long)gpu_frame()); }
static void audit_draw(void) {
    uint32_t tc = reg[0x080];
    if ((tc >> 10) & 1) {
        for (int i = 0; i < 6; i++) { uint32_t src = reg[tev_reg[i]]; int hit = 0;
            for (int j = 0; j < 3; j++) if (((src >> (4 * j)) & 15) == 6 || ((src >> (16 + 4 * j)) & 15) == 6) hit = 1;
            if (hit) { audit_hit(A_PROCTEX); break; } }
    }
    if (tc & 1) { uint32_t type = (reg[0x083] >> 28) & 7;
        if (type == 1) audit_hit(A_TEX_CUBE); else if (type == 2) audit_hit(A_TEX_SHADOW); else if (type == 4) audit_hit(A_TEX_SHADOWCUBE); }
    if ((reg[0x0e0] & 7) == 7) audit_hit(A_GAS_FOG);
    if ((reg[0x100] & 3) == 1) audit_hit(A_FRAG_GAS);
    if ((reg[0x100] & 3) == 3) audit_hit(A_FRAG_SHADOW);
    uint32_t bl = reg[0x101];
    if (reg[0x100] & 0x100) {
        int rgb = (bl & 7) == 3 || (bl & 7) == 4, a = ((bl >> 8) & 7) == 3 || ((bl >> 8) & 7) == 4;
        if ((rgb && (((bl >> 16) & 15) != 1 || ((bl >> 20) & 15) != 1)) || (a && (((bl >> 24) & 15) != 1 || ((bl >> 28) & 15) != 1))) audit_hit(A_MINMAX_FACTORS);
        uint32_t cf = (reg[0x117] >> 16) & 7;
        if (cf >= 2 && cf <= 4) audit_hit(A_TARGET_16BIT_BLEND);
    } else if ((reg[0x102] & 15) != 3) audit_hit(A_LOGIC_OP);
    static const uint32_t base_reg[3] = { 0x081, 0x091, 0x099 };
    for (int u = 0; u < 3; u++) {
        if (!((tc >> u) & 1)) continue;
        uint32_t prm = reg[base_reg[u] + 2], lod = reg[base_reg[u] + 3];
        if (((prm >> 8) & 7) >= 4 || ((prm >> 12) & 7) >= 4) audit_hit(A_WRAP_ODD);
        if ((lod & 0x1fff) && ((lod >> 16) & 15)) audit_hit(A_LOD_BIAS);
    }
}

/* ---- geometry shader: point mode (as Azahar's GeometryPipeline_Point) ----
 * The vertex shader's outputs (the first vs_outmap_total of those its output mask enables, in register order) are
 * collected; every gs_input_count of them become the geometry shader's inputs, placed by its input map. SETEMIT and
 * EMIT then produce vertices, mapped through the output semantics like a vertex shader's; an EMIT with the primitive
 * flag set completes a triangle of the three slots (winding swaps the first two). */
/* Done in two parallel steps: every distinct vertex is shaded once (its outputs kept raw), then the invocations run
 * in chunks, each chunk collecting its triangles in its own list; the lists are rasterised in order afterwards, so
 * the result is the same as running everything in sequence. */
static void triangle(const Fb *fb, const Vtx *a, const Vtx *b, const Vtx *c);
typedef struct { Vtx *v; uint32_t n, cap; } GsOut;          /* triangles of one chunk: 3 vertices each */
typedef struct { PicaEmitter em; Vtx slot[3]; GsOut *out; } GsEm;
static float (*gs_raw)[16][4];                              /* raw vertex shader outputs, compacted, by vertex index */
static uint32_t gs_vs_n, gs_omask, gs_n_in, gs_vpi, gs_ninv, gs_count, gs_first, gs_i16, gs_indexed, gs_next;
static const uint8_t *gs_ib;
static uint64_t gs_map;
static GsOut *gs_outs; static uint32_t gs_nouts;
#define GS_CHUNK 16
static void gs_emit(PicaEmitter *em_, const float o[16][4]) {
    GsEm *em = (GsEm *)em_;
    if (em_->vertex_id > 2) return;
    map_outputs(o, reg[0x28d] & 0xffff, &em->slot[em_->vertex_id]);
    if (!em_->prim_emit) return;
    GsOut *out = em->out;
    if (out->n + 3 > out->cap) { out->cap = out->cap ? out->cap * 2 : 96; out->v = realloc(out->v, out->cap * sizeof *out->v); if (!out->v) fatal("out of memory"); }
    int w = em_->winding;
    out->v[out->n++] = em->slot[w ? 1 : 0]; out->v[out->n++] = em->slot[w ? 0 : 1]; out->v[out->n++] = em->slot[2];
}
static void gs_shade_raw(uint32_t base, uint32_t vi) {
    float in[16][4] = { { 0 } }, o[16][4] = { { 0 } };
    load_vertex(base, vi, in);
    pica_shader_run(&vs, reg[0x2ba] & 0xffff, in, o);
    for (uint32_t r = 0, k = 0; r < 16 && k < gs_vs_n; r++)
        if ((gs_omask >> r) & 1) memcpy(gs_raw[vi][k++], o[r], sizeof o[r]);
}
static void run_gs_vertices(void) {
    for (;;) {
        uint32_t i = __atomic_fetch_add(&job_next, 32, __ATOMIC_RELAXED);
        if (i >= job_nv) break;
        uint32_t e = i + 32 < job_nv ? i + 32 : job_nv;
        for (; i < e; i++) gs_shade_raw(job_base, job_vlist[i]);
    }
}
static uint32_t gs_index(uint32_t i) {
    return gs_indexed ? (gs_i16 ? (uint32_t)(gs_ib[2 * i] | gs_ib[2 * i + 1] << 8) : gs_ib[i]) : gs_first + i;
}
static void run_gs_invocations(void) {
    GsEm em; memset(&em, 0, sizeof em); em.em.emit = gs_emit; em.em.own_bools = 1;
    for (;;) {
        uint32_t c = __atomic_fetch_add(&gs_next, 1, __ATOMIC_RELAXED);
        if (c >= gs_nouts) break;
        em.out = &gs_outs[c]; em.out->n = 0;
        uint32_t lo = c * GS_CHUNK, hi = lo + GS_CHUNK < gs_ninv ? lo + GS_CHUNK : gs_ninv;
        for (uint32_t inv = lo; inv < hi; inv++) {
            float gin[16][4] = { { 0 } }, gout[16][4] = { { 0 } };
            uint32_t a = 0;
            for (uint32_t v = 0; v < gs_vpi; v++) {
                uint32_t vi = gs_index(inv * gs_vpi + v);
                for (uint32_t k = 0; k < gs_vs_n && a < gs_n_in; k++, a++) memcpy(gin[(gs_map >> (4 * a)) & 15], gs_raw[vi][k], sizeof gin[0]);
            }
            em.em.vertex_id = 0; em.em.prim_emit = 0; em.em.winding = 0;
            em.em.bools = gs.bools | (inv ? 0x8000u : 0);          /* b15 is set after every invocation (Azahar) */
            if (pica_shader_run_gs(&gs, reg[0x28a] & 0xffff, gin, gout, &em.em) < 0) LOG(1, "pica: geometry shader did not finish");
        }
    }
}
static void run_job(int b) {
    switch (job_kind) {
    case 0: run_band(b); break;
    case 1: run_vertices(b, nworkers); break;
    case 2: run_gs_vertices(); break;
    default: run_gs_invocations();
    }
}
static uint64_t n_gs_runs;
static void pool_run(int kind);
static void gs_draw(const Fb *fb, uint32_t base, const uint8_t *ib, int i16, int indexed, uint32_t first, uint32_t count,
                    const uint32_t *vlist, uint32_t nuniq) {
    gs.bools = (reg[0x280] & 0xffff) | (gs_b15 ? 0x8000u : 0);
    for (int k = 0; k < 4; k++) gs.ints[k] = reg[0x281 + k];
    pica_shader_prepare_ver(&gs, gs_ver);
    gs_vs_n = (reg[0x24a] & 15) + 1; gs_n_in = (reg[0x289] & 15) + 1; gs_omask = reg[0x2bd] & 0xffff;
    gs_map = (uint64_t)reg[0x28c] << 32 | reg[0x28b];
    gs_vpi = gs_n_in / gs_vs_n;
    if (!gs_vpi || gs_n_in % gs_vs_n) { if (!n_skipped_gs++) LOG(1, "pica: geometry shader inputs (%u) are not whole vertices (%u outputs each)", gs_n_in, gs_vs_n); return; }
    if (!gs_raw) { gs_raw = malloc(65536 * sizeof *gs_raw); if (!gs_raw) fatal("out of memory"); }
    pool_init();
    /* 1: every distinct vertex once */
    job_base = base; job_vlist = vlist; job_nv = nuniq; job_next = 0;
    if (nworkers == 1 || nuniq < 48) { job_kind = 2; run_gs_vertices(); } else pool_run(2);
    /* 2: the invocations, in chunks */
    gs_ib = ib; gs_i16 = (uint32_t)i16; gs_indexed = (uint32_t)indexed; gs_first = first; gs_count = count;
    gs_ninv = count / gs_vpi; n_gs_runs += gs_ninv;
    uint32_t nchunks = (gs_ninv + GS_CHUNK - 1) / GS_CHUNK;
    static uint32_t outs_cap;
    if (nchunks > outs_cap) { gs_outs = realloc(gs_outs, nchunks * sizeof *gs_outs); memset(gs_outs + outs_cap, 0, (nchunks - outs_cap) * sizeof *gs_outs); outs_cap = nchunks; }
    gs_nouts = nchunks; gs_next = 0;
    if (nworkers == 1 || nchunks < 4) { job_kind = 3; run_gs_invocations(); } else pool_run(3);
    /* 3: rasterise in order */
    for (uint32_t c = 0; c < nchunks; c++)
        for (uint32_t t = 0; t + 2 < gs_outs[c].n; t += 3) triangle(fb, &gs_outs[c].v[t], &gs_outs[c].v[t + 1], &gs_outs[c].v[t + 2]);
    if (gs_ninv) gs_b15 = 1;
}
/* Geometry shader modes whose inputs go into the unit's float uniforms (0x289 bit 8, register 0x252 mode 1 or 2), as
 * Azahar does: one invocation after another, each sees what the previous ones left in the uniforms.
 *   fixed primitive (2): vertices fill c[start..] (0x252 bits 16-23) with all their outputs, 0x252 bits 8-11 + 1 vertices
 *     per invocation;
 *   variable primitive (1): in an indexed draw, an index is a vertex count n instead of a vertex: c0 = (n, n, n, n), then
 *     the next n vertices fill c1.., the first 0x254 + 1 with all their outputs, the rest with their first one only. */
static void gs_draw_uniform(const Fb *fb, uint32_t base, const uint8_t *ib, int i16, int indexed, uint32_t first, uint32_t count,
                            const uint32_t *vlist, uint32_t nuniq, int mode) {
    gs.bools = (reg[0x280] & 0xffff) | (gs_b15 ? 0x8000u : 0);
    for (int k = 0; k < 4; k++) gs.ints[k] = reg[0x281 + k];
    pica_shader_prepare_ver(&gs, gs_ver);
    gs_vs_n = (reg[0x24a] & 15) + 1; gs_omask = reg[0x2bd] & 0xffff;
    if (mode == 1 && !indexed) { if (!n_skipped_gs++) LOG(1, "pica: variable-primitive geometry shader in a draw without indices (skipped)"); return; }
    if (!gs_raw) { gs_raw = malloc(65536 * sizeof *gs_raw); if (!gs_raw) fatal("out of memory"); }
    pool_init();
    job_base = base; job_vlist = vlist; job_nv = nuniq; job_next = 0;
    if (nworkers == 1 || nuniq < 48) { job_kind = 2; run_gs_vertices(); } else pool_run(2);
    gs_ib = ib; gs_i16 = (uint32_t)i16; gs_indexed = (uint32_t)indexed; gs_first = first;
    static GsOut out;
    GsEm em; memset(&em, 0, sizeof em); em.em.emit = gs_emit; em.out = &out; out.n = 0;
    uint32_t start = (reg[0x252] >> 16) & 0xff, nfix = ((reg[0x252] >> 8) & 15) + 1, pos = 0, need_index = mode == 1, main_left = 0, total_left = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t vi = gs_index(i);
        int full = 0;
        if (mode == 1) {
            if (need_index) {
                for (int k = 0; k < 4; k++) gs.uniform[0][k] = (float)vi;
                pos = 1; main_left = reg[0x254] + 1; total_left = vi; need_index = 0;
                continue;
            }
            if (main_left) { for (uint32_t k = 0; k < gs_vs_n && pos < 96; k++) memcpy(gs.uniform[pos++], gs_raw[vi][k], sizeof gs.uniform[0]); main_left--; }
            else if (pos < 96) memcpy(gs.uniform[pos++], gs_raw[vi][0], sizeof gs.uniform[0]);
            if (--total_left == 0) { need_index = 1; full = 1; }
        } else {
            for (uint32_t k = 0; k < gs_vs_n && start + pos < 96; k++) memcpy(gs.uniform[start + pos++], gs_raw[vi][k], sizeof gs.uniform[0]);
            if (pos >= gs_vs_n * nfix) { pos = 0; full = 1; }
        }
        if (!full) continue;
        float gin[16][4] = { { 0 } }, gout[16][4] = { { 0 } };
        em.em.vertex_id = 0; em.em.prim_emit = 0; em.em.winding = 0;
        if (pica_shader_run_gs(&gs, reg[0x28a] & 0xffff, gin, gout, &em.em) < 0) LOG(1, "pica: geometry shader did not finish");
        gs.bools |= 0x8000u; gs_b15 = 1; n_gs_runs++;
    }
    for (uint32_t t = 0; t + 2 < out.n; t += 3) triangle(fb, &out.v[t], &out.v[t + 1], &out.v[t + 2]);
}

static void draw_immediate(void) {
    static Vtx buf[3], first_v;
    if ((reg[0x229] & 3) == 2) { if (!n_skipped_gs++) LOG(1, "pica: immediate-mode drawing through a geometry shader is not implemented"); return; }
    { static int tried; if (!tried) { tried = 1; vkr_init(); } }
    Fb fb;
    fb.width = reg[0x11e] & 0x7ff; fb.hreg = (reg[0x11e] >> 12) & 0x3ff;
    fb.cfmt = (reg[0x117] >> 16) & 7; fb.dfmt = reg[0x116] & 3;
    fb.cbpp = fb.cfmt == 0 ? 4 : fb.cfmt == 1 ? 3 : 2; fb.dbpp = fb.dfmt == 0 ? 2 : fb.dfmt == 2 ? 3 : 4;
    uint32_t caddr = (reg[0x11d] & 0x0fffffff) << 3, daddr = (reg[0x11c] & 0x0fffffff) << 3;
    fb.color = phys(caddr, fb.width * (fb.hreg + 1) * fb.cbpp);
    fb.depth = phys(daddr, fb.width * (fb.hreg + 1) * fb.dbpp);
    /* the vertex: attributes to shader inputs as the loader does (0x2bb/0x2bc), then the vertex shader */
    float in[16][4] = { { 0 } }, o[16][4] = { { 0 } };
    uint32_t nin = (reg[0x2b9] & 15) + 1, nattr = (reg[0x242] & 15) + 1;
    uint64_t map = (uint64_t)reg[0x2bc] << 32 | reg[0x2bb];
    for (uint32_t a = 0; a < nin && a < nattr; a++) memcpy(in[(map >> (4 * a)) & 15], imm_attr[a], sizeof in[0]);
    vs.bools = reg[0x2b0] & 0xffff;
    for (int k = 0; k < 4; k++) vs.ints[k] = reg[0x2b1 + k];
    pica_shader_prepare_ver(&vs, vs_ver);
    if (pica_shader_run(&vs, reg[0x2ba] & 0xffff, in, o) < 0) LOG(1, "pica: vertex shader did not finish");
    Vtx v; map_outputs(o, reg[0x2bd] & 0xffff, &v);
    uint32_t topo = (reg[0x25e] >> 8) & 3;
    const Vtx *t0 = 0, *t1 = 0;
    Vtx tri_a, tri_b;
    if (topo == 0 || topo == 3) {
        buf[imm_nb++] = v;
        if (imm_nb == 3) { tri_a = buf[0]; tri_b = buf[1]; t0 = &tri_a; t1 = &tri_b; v = buf[2]; imm_nb = 0; }
    } else if (topo == 1) {
        if (imm_nb == 2) { tri_a = imm_odd ? buf[1] : buf[0]; tri_b = imm_odd ? buf[0] : buf[1]; t0 = &tri_a; t1 = &tri_b; imm_odd ^= 1; buf[0] = buf[1]; buf[1] = v; }
        else buf[imm_nb++] = v;
    } else {
        if (imm_nb == 0) { first_v = v; buf[0] = v; imm_nb = 1; }
        else if (imm_nb == 1) { buf[1] = v; imm_nb = 2; }
        else { tri_a = first_v; tri_b = buf[1]; t0 = &tri_a; t1 = &tri_b; buf[1] = v; }
    }
    if (!t0) return;
    n_draws++;
    if (flat_mode < 0) flat_mode = getenv("RB_FLAT") != 0;
    for (int u = 0; u < 3; u++) setup_tex(&tu[u], u);
    setup_tev();
    setup_lighting();
    triangle(&fb, t0, t1, &v);
    if (vkr_on) { vk_submit(&fb, caddr, daddr); ntris = 0; }
    else flush_tris(&fb);
}

static void draw(int indexed) {
    { static int tried; if (!tried) { tried = 1; vkr_init(); } }
    uint64_t t_draw0 = pns();
    n_draws++;
    { static int64_t stop = -2; if (stop == -2) { const char *e = getenv("RB_STOP_AFTER_DRAW"); stop = e ? atoll(e) : -1; }
      if (stop >= 0 && (int64_t)n_draws > stop) return; }                 /* replay debugging: skip later draws */
    if (log_draws < 0) { const char *s = getenv("RB_PICA_LOG"); log_draws = s ? atoi(s) : 0; s = getenv("RB_PICA_LOG_FROM"); log_from = s ? atoll(s) : 0;
                         s = getenv("RB_LOG_FRAME"); log_frame = s ? atoll(s) : -1; }
    if (gpu_frame() != frame_seen) { frame_seen = gpu_frame(); frame_draw_base = n_draws; }
    uint32_t draw_in_frame = (uint32_t)(n_draws - frame_draw_base);
    int use_gs = (reg[0x229] & 3) == 2;
    int gs_mode = use_gs ? (int)(reg[0x252] & 0xff) : 0;              /* 0 point, 1 variable primitive, 2 fixed primitive */
    if (gs_mode > 2) { if (!n_skipped_gs++) LOG(1, "pica: geometry shader mode %d is unknown (draw skipped)", gs_mode); return; }
    Fb fb;
    fb.width = reg[0x11e] & 0x7ff; fb.hreg = (reg[0x11e] >> 12) & 0x3ff;
    fb.cfmt = (reg[0x117] >> 16) & 7; fb.dfmt = reg[0x116] & 3;
    fb.cbpp = fb.cfmt == 0 ? 4 : fb.cfmt == 1 ? 3 : 2; fb.dbpp = fb.dfmt == 0 ? 2 : fb.dfmt == 2 ? 3 : 4;
    uint32_t caddr = (reg[0x11d] & 0x0fffffff) << 3, daddr = (reg[0x11c] & 0x0fffffff) << 3;
    fb.color = phys(caddr, fb.width * (fb.hreg + 1) * fb.cbpp);
    fb.depth = phys(daddr, fb.width * (fb.hreg + 1) * fb.dbpp);
    if (flat_mode < 0) flat_mode = getenv("RB_FLAT") != 0;
    for (int u = 0; u < 3; u++) setup_tex(&tu[u], u);
    setup_tev();
    setup_lighting();
    audit_draw();
    if ((reg[0x0e0] & 7) == 5) { if (!n_fog_draws++) LOG(1, "pica: first fog draw at frame %llu", (unsigned long long)gpu_frame()); }
    if ((reg[0x0e0] & 7) == 7 && !warned_tex[7]) { warned_tex[7] = 1; LOG(1, "pica: gas rendering (fog mode 7) is not implemented"); }
    uint32_t h = draw_in_frame * 2654435761u;                       /* colour depends only on the position in the frame */
    flat[0] = (uint8_t)(64 + (h >> 24) % 192); flat[1] = (uint8_t)(64 + (h >> 16) % 192); flat[2] = (uint8_t)(64 + (h >> 8) % 192); flat[3] = 255;

    uint32_t base = (reg[0x200] & 0x1ffffffe) << 3, count = reg[0x228], first = reg[0x22a], topo = (reg[0x25e] >> 8) & 3;
    const uint8_t *ib = 0; int i16 = (int)(reg[0x227] >> 31);
    if (indexed) {
        ib = phys(base + (reg[0x227] & 0x0fffffff), count * (i16 ? 2 : 1));
        if (!ib) { LOG(1, "pica: index buffer at physical %08x is not mapped", base + (reg[0x227] & 0x0fffffff)); return; }
    }
    memset(vtx_out, 0, sizeof vtx_out); vtx_total = 0; ndc_min[0] = ndc_min[1] = 1e30f; ndc_max[0] = ndc_max[1] = -1e30f; frag_cov = frag_alpha = frag_depth = frag_stencil = 0;
    uint64_t tris_before = n_tris, px_before = n_pixels, in_before = n_in_tris, clip_before = n_clipped, cull_before = n_culled;
    if (dump_draw < 0) { const char *s = getenv("RB_PICA_DUMP"); dump_draw = s ? atoi(s) : 0; }
    int dumping = (int64_t)n_draws == dump_draw;
    if (dumping) dump_left = 6;
    Vtx buf[3]; int nb = 0, odd = 0;
    Vtx first_v;
    vs.bools = reg[0x2b0] & 0xffff;
    for (int k = 0; k < 4; k++) vs.ints[k] = reg[0x2b1 + k];
    /* Shade every distinct vertex once (indexed draws reuse indices; the shader output depends only on the
     * vertex and the draw's uniforms), in parallel, then assemble primitives in order. */
    static Vtx *vcache; static uint32_t *vstamp, *vlist; static uint32_t vgen, vlist_cap;
    if (!vcache) { vcache = malloc(65536 * sizeof *vcache); vstamp = calloc(65536, sizeof *vstamp); if (!vcache || !vstamp) fatal("out of memory"); }
    if (++vgen == 0) { memset(vstamp, 0, 65536 * sizeof *vstamp); vgen = 1; }
    if (count > vlist_cap) { vlist_cap = count; vlist = realloc(vlist, count * sizeof *vlist); if (!vlist) fatal("out of memory"); }
    uint32_t nuniq = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t vi = indexed ? (i16 ? (uint32_t)(ib[2 * i] | ib[2 * i + 1] << 8) : ib[i]) : first + i;
        if (vi > 0xffff) fatal("vertex index %u out of range", vi);
        if (vstamp[vi] != vgen) { vstamp[vi] = vgen; vlist[nuniq++] = vi; }
    }
    uint64_t tA = pns(); t_setup += tA - t_draw0;
    pica_shader_prepare_ver(&vs, vs_ver);
    if (use_gs) {                                                     /* geometry shader */
        if (gs_mode == 0) gs_draw(&fb, base, ib, i16, indexed, first, count, vlist, nuniq);
        else gs_draw_uniform(&fb, base, ib, i16, indexed, first, count, vlist, nuniq, gs_mode);
        uint64_t tC = pns(); t_shade += tC - tA;
        if (vkr_on) { vk_submit(&fb, caddr, daddr); ntris = 0; }
        else flush_tris(&fb);
        t_raster += pns() - tC;
        return;
    }
    n_shaded += nuniq;
    shade_vertices(base, vlist, nuniq, vcache);
    uint64_t tB = pns(); t_shade += tB - tA;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t vi = indexed ? (i16 ? (uint32_t)(ib[2 * i] | ib[2 * i + 1] << 8) : ib[i]) : first + i;
        Vtx v = vcache[vi];
        if (dumping && i < 4) dump_vertex(base, vi, &v);
        if (dumping && getenv("RB_DUMP_ALLV")) { float in[16][4] = { { 0 } }; load_vertex(base, vi, in);
            fprintf(stderr, "V %u %g %g %g | %g %g %g %g | col %.3f %.3f %.3f %.3f tc0 %.3f %.3f tc1 %.3f %.3f tc2 %.3f %.3f tc0w %.3f\n", vi, in[0][0], in[0][1], in[0][2], v.a[0], v.a[1], v.a[2], v.a[3],
                    v.a[8], v.a[9], v.a[10], v.a[11], v.a[12], v.a[13], v.a[14], v.a[15], v.a[22], v.a[23], v.a[16]); }
        vtx_total++;
        if (v.a[3] > 0) for (int k = 0; k < 2; k++) { float q = v.a[k] / v.a[3]; if (q < ndc_min[k]) ndc_min[k] = q; if (q > ndc_max[k]) ndc_max[k] = q; }
        { float *p = v.a; if (p[0] > p[3]) vtx_out[0]++; if (p[0] < -p[3]) vtx_out[1]++; if (p[1] > p[3]) vtx_out[2]++; if (p[1] < -p[3]) vtx_out[3]++;
          if (p[2] > 0) vtx_out[4]++; if (p[2] < -p[3]) vtx_out[5]++; if (p[3] <= 0) vtx_out[6]++; }
        if (topo == 0 || topo == 3) {
            buf[nb++] = v;
            if (nb == 3) { triangle(&fb, &buf[0], &buf[1], &buf[2]); nb = 0; }
        } else if (topo == 1) {
            if (nb == 2) { if (odd) triangle(&fb, &buf[1], &buf[0], &v); else triangle(&fb, &buf[0], &buf[1], &v); odd ^= 1; buf[0] = buf[1]; buf[1] = v; }
            else buf[nb++] = v;
        } else {
            if (i == 0) first_v = v;
            else if (nb == 1) { triangle(&fb, &first_v, &buf[0], &v); buf[0] = v; }
            else { buf[0] = v; nb = 1; }
        }
    }
    uint64_t tC = pns(); t_assemble += tC - tB;
    if (vkr_on) { vk_submit(&fb, caddr, daddr); ntris = 0; }
    else flush_tris(&fb);
    t_raster += pns() - tC;
    if (((int64_t)n_draws > log_from && (int64_t)n_draws <= log_from + log_draws) || (int64_t)gpu_frame() == log_frame) {
        LOG(1, "pica: frame %llu draw #%u flat colour %02x%02x%02x  list %08x (running %08x)", (unsigned long long)gpu_frame(), draw_in_frame, flat[0], flat[1], flat[2], top_list, cur_list);
        LOG(1, "pica: draw %llu %s count %u topo %u entry %03x outmask %04x outputs %u | fb %ux%u fmt %u at %08x%s depth fmt %u at %08x%s | tex %x fmt %u/%u/%u lighting %u | %llu triangles, %llu pixels",
            (unsigned long long)n_draws, indexed ? "indexed" : "arrays", count, topo, reg[0x2ba] & 0xffff, reg[0x2bd] & 0xffff, reg[0x04f] & 7,
            fb.width, fb.hreg + 1, fb.cfmt, caddr, fb.color ? "" : " (unmapped)", fb.dfmt, daddr, fb.depth ? "" : " (unmapped)",
            reg[0x080] & 7, reg[0x08e] & 15, reg[0x096] & 15, reg[0x09e] & 15, reg[0x08f] & 1,
            (unsigned long long)(n_tris - tris_before), (unsigned long long)(n_pixels - px_before));
        LOG(1, "      %llu triangles in, %llu clipped away, %llu culled | vertices %llu: outside x>w %llu x<-w %llu y>w %llu y<-w %llu z>0 %llu z<-w %llu | c86 (%g %g %g %g) c87 (%g %g %g %g) fixed3 (%g %g %g %g) viewport %g x %g corner %08x",
            (unsigned long long)(n_in_tris - in_before), (unsigned long long)(n_clipped - clip_before), (unsigned long long)(n_culled - cull_before),
            (unsigned long long)vtx_total, (unsigned long long)vtx_out[0], (unsigned long long)vtx_out[1], (unsigned long long)vtx_out[2], (unsigned long long)vtx_out[3],
            (unsigned long long)vtx_out[4], (unsigned long long)vtx_out[5], vs.uniform[86][0], vs.uniform[86][1], vs.uniform[86][2], vs.uniform[86][3],
            vs.uniform[87][0], vs.uniform[87][1], vs.uniform[87][2], vs.uniform[87][3], dflt[3][0], dflt[3][1], dflt[3][2], dflt[3][3],
            f24(reg[0x041]), f24(reg[0x043]), reg[0x068]);
        LOG(1, "      fragments covered %llu, alpha-tested away %llu (alpha test %08x), depth-failed %llu, stencil-failed %llu | depth range %g near %g zbuffer %u | write mask %x",
            (unsigned long long)frag_cov, (unsigned long long)frag_alpha, reg[0x104], (unsigned long long)frag_depth, (unsigned long long)frag_stencil,
            f24(reg[0x04d]), f24(reg[0x04e]), reg[0x06d] & 1, (reg[0x107] >> 8) & 31);
        LOG(1, "      uniform c6 uploaded from %08lx (raw %08x %08x %08x %08x), c86 from %08lx, c25 from %08lx", (unsigned long)uni_src[6],
            uni_src[6] ? ((uint32_t *)uni_src[6])[0] : 0, uni_src[6] ? ((uint32_t *)uni_src[6])[1] : 0, uni_src[6] ? ((uint32_t *)uni_src[6])[2] : 0, uni_src[6] ? ((uint32_t *)uni_src[6])[3] : 0,
            (unsigned long)uni_src[86], (unsigned long)uni_src[25]);
        LOG(1, "      clip-space extent: x/w %.3f .. %.3f, y/w %.3f .. %.3f", ndc_min[0], ndc_max[0], ndc_min[1], ndc_max[1]);
        LOG(1, "      bones c25 (%g %g %g %g) c26 (%g %g %g %g) c28 (%g %g %g %g) c31 (%g %g %g %g)", vs.uniform[25][0], vs.uniform[25][1], vs.uniform[25][2], vs.uniform[25][3],
            vs.uniform[26][0], vs.uniform[26][1], vs.uniform[26][2], vs.uniform[26][3], vs.uniform[28][0], vs.uniform[28][1], vs.uniform[28][2], vs.uniform[28][3],
            vs.uniform[31][0], vs.uniform[31][1], vs.uniform[31][2], vs.uniform[31][3]);
    }
}

/* Full PICA state (registers, shader program and uniforms, fixed attributes) for the replay test. */
typedef struct { uint32_t magic, version; uint32_t reg[0x300]; PicaShader vs; float dflt[16][4]; uint32_t lut[24][256];
                 uint32_t uni_queue[4]; int32_t uni_n; uint32_t dflt_queue[3]; int32_t dflt_n; } PicaState;
int pica_save_state(const char *path) {
    static PicaState st;
    st.magic = 0x50494341; st.version = 2; memcpy(st.lut, lut, sizeof lut);
    memcpy(st.reg, reg, sizeof reg); st.vs = vs; memcpy(st.dflt, dflt, sizeof dflt);
    memcpy(st.uni_queue, uni_queue, sizeof uni_queue); st.uni_n = uni_n; memcpy(st.dflt_queue, dflt_queue, sizeof dflt_queue); st.dflt_n = dflt_n;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(&st, sizeof st, 1, f); fclose(f);
    return 0;
}
int pica_load_state(const char *path) {
    static PicaState st;
    FILE *f = fopen(path, "rb");
    if (!f || fread(&st, sizeof st, 1, f) != 1 || st.magic != 0x50494341) { if (f) fclose(f); return -1; }
    fclose(f);
    if (st.version != 2) return -1;
    memcpy(reg, st.reg, sizeof reg); vs = st.vs; vs_ver++; gs_ver++; memcpy(dflt, st.dflt, sizeof dflt); memcpy(lut, st.lut, sizeof lut);
    memcpy(uni_queue, st.uni_queue, sizeof uni_queue); uni_n = st.uni_n; memcpy(dflt_queue, st.dflt_queue, sizeof dflt_queue); dflt_n = st.dflt_n;
    return 0;
}

void pica_report(void) {
    fprintf(stderr, "pica: time in draws: setup %.1f ms, vertex shading %.1f ms, assembly+clipping %.1f ms, rasterisation %.1f ms\n",
            (double)t_setup / 1e6, (double)t_shade / 1e6, (double)t_assemble / 1e6, (double)t_raster / 1e6);
    fprintf(stderr, "pica: uniform index writes: %llu float24, %llu float32, %llu left a partial vector\n",
            (unsigned long long)n_f24_uni, (unsigned long long)n_f32_uni, (unsigned long long)n_partial_uni);
    fprintf(stderr, "pica: writes to geometry-shader registers 0x280-0x2af:");
    for (int r = 0x280; r < 0x2b0; r++) if (reg_writes[r]) fprintf(stderr, " %03x:%llu", r, (unsigned long long)reg_writes[r]);
    fprintf(stderr, "\npica: writes to vertex-shader registers:");
    for (int r = 0x2b0; r < 0x2e0; r++) if (reg_writes[r]) fprintf(stderr, " %03x:%llu", r, (unsigned long long)reg_writes[r]);
    fprintf(stderr, "\npica: other pipeline registers written:");
    for (int r = 0x229; r < 0x260; r++) if (reg_writes[r]) fprintf(stderr, " %03x:%llu", r, (unsigned long long)reg_writes[r]);
    fputc('\n', stderr);
    fprintf(stderr, "pica: %llu draws with fog\n", (unsigned long long)n_fog_draws);
    fprintf(stderr, "pica: audit of missing or approximated features (draws using each):\n");
    for (int k = 0; k < A_COUNT; k++) fprintf(stderr, "  %-40s %llu\n", audit_name[k], (unsigned long long)audit[k]);
    fprintf(stderr, "pica: %llu geometry shader runs\n", (unsigned long long)n_gs_runs);
    fprintf(stderr, "pica: %llu command lists, %llu draws, %llu triangles rasterised, %llu pixels written, %llu geometry-shader draws skipped\n",
            (unsigned long long)n_lists, (unsigned long long)n_draws, (unsigned long long)n_tris, (unsigned long long)n_pixels, (unsigned long long)n_skipped_gs);
}


/* ---- save states: the registers and everything uploaded through them; decoded caches start over ---- */
void pica_state(SS *s) {
    ss_tag(s, "pica");
    SS_V(s, reg); SS_V(s, lut); SS_V(s, fog_lut); SS_V(s, pt_lut); SS_V(s, uni_src); SS_V(s, vs); SS_V(s, gs);
    SS_V(s, uni_queue); SS_V(s, uni_n); SS_V(s, gs_uni_queue); SS_V(s, gs_uni_n); SS_V(s, dflt); SS_V(s, dflt_queue); SS_V(s, dflt_n);
    SS_V(s, tevs); SS_V(s, tev_buf_init); SS_V(s, lights); SS_V(s, nlights); SS_V(s, global_amb); SS_V(s, tu);
    SS_V(s, flat_mode); SS_V(s, flat); SS_V(s, gs_b15); SS_V(s, imm_attr); SS_V(s, imm_cur);
    if (ss_loading(s)) { vs_ver += 1000000; gs_ver += 1000000; lut_version += 1000000; }
}
