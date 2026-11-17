/* Runtime support for statically recompiled ARM11 (ARMv6K + VFPv2) code.
 * Guest memory is mapped at its original addresses, so a guest pointer is a host pointer. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, cf, v, q;   /* CPSR flags, each 0 or 1 */
    uint32_t s[32];            /* VFP registers as raw bits; D[i] = s[2i] | s[2i+1] << 32 */
    uint32_t fpscr;
    uint32_t tls;              /* cp15 thread register (TPIDRURO) */
    uint32_t ge;               /* CPSR.GE, one bit per byte lane */
} CPU;

typedef void (*recomp_fn)(CPU *);

/* provided by the host program (runtime or test runner) */
void recomp_trap(CPU *c, uint32_t addr, uint32_t info, const char *why) __attribute__((noreturn));
void call_indirect(CPU *c, uint32_t addr);
void svc_call(CPU *c, uint32_t num, uint32_t addr);
void vfp_set_fpscr(CPU *c, uint32_t v);
float vfp_nan2f(float a, float b);
float vfp_nan1f(float a);
double vfp_nan2d(double a, double b);
double vfp_nan1d(double a);

#define TRAP(addr, why) recomp_trap(c, (addr), 0, (why))
#define TRAP_UNIMPL(addr, word) recomp_trap(c, (addr), (word), "unimplemented instruction")

/* Function tables: ARM functions one slot per 4 bytes of guest .text, Thumb functions one slot per 2 bytes.
 * A Thumb function is addressed with bit 0 set, as an interworking branch (bx, blx, pop {pc}) carries it. */
/* RECOMP_TEXT_BASE / RECOMP_TEXT_SIZE and the other layout values: generated from the ExHeader */
#include "layout.h"
extern recomp_fn *recomp_table, *recomp_table_thumb;
static inline recomp_fn recomp_lookup(uint32_t a) {
    uint32_t o = (a & ~1u) - RECOMP_TEXT_BASE;
    if (o >= RECOMP_TEXT_SIZE) return (recomp_fn)0;
    if (a & 1) return recomp_table_thumb[o >> 1];
    return (a & 3) ? (recomp_fn)0 : recomp_table[o >> 2];
}
static inline void recomp_table_build(const void *list, uint32_t count, void *(*alloc)(size_t, size_t)) {
    const struct { uint32_t addr; recomp_fn fn; } *f = list;
    recomp_table = alloc(RECOMP_TEXT_SIZE / 4, sizeof(recomp_fn));
    recomp_table_thumb = alloc(RECOMP_TEXT_SIZE / 2, sizeof(recomp_fn));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t o = (f[i].addr & ~1u) - RECOMP_TEXT_BASE;
        if (f[i].addr & 1) recomp_table_thumb[o >> 1] = f[i].fn; else recomp_table[o >> 2] = f[i].fn;
    }
}

/* Loop back-edges count down recomp_budget; at zero the runtime may let another core's thread run (kernel.c). */
#define PREEMPT_BUDGET 25000
extern int recomp_budget;
void recomp_preempt(CPU *c);
#define BACKEDGE() do { if (__builtin_expect(--recomp_budget < 0, 0)) recomp_preempt(c); } while (0)

/* Returns record the guest return target in r15. Builds with RECOMP_CHECK_RETURNS verify at
 * every call site that the callee came back to the instruction after the call. */
#define RET(t) do { c->r[15] = (t); return; } while (0)
/* Address of the last computed return/jump, for diagnosing returns to unexpected places. */
extern uint32_t recomp_site;
#ifdef RECOMP_CHECK_RETURNS
#define SITE(a) (recomp_site = (a))
#else
#define SITE(a) ((void)0)
#endif
/* Computed jump that is not an obvious return: a jump to the caller's address is a return,
 * a jump to a known function is a tail call, anything else is a return to an outer frame. */
#define IJUMP(t) do { uint32_t t_ = (t); c->r[15] = t_; if (t_ == ret_lr) return; \
    recomp_fn f_ = recomp_lookup(t_); if (f_) f_(c); return; } while (0)
/* Same, inside a function that has hand-made return addresses: after the tail call, look at where
 * the callee returned to (L_resume) because it may be a local address. */
#define IJUMP_R(t) do { uint32_t t_ = (t); c->r[15] = t_; if (t_ == ret_lr) return; \
    recomp_fn f_ = recomp_lookup(t_); if (f_) f_(c); goto L_resume; } while (0)
/* Return inside a function with hand-made return addresses: the target may be a local label. */
#define RET_R(t) do { c->r[15] = (t); goto L_resume; } while (0)
#ifdef RECOMP_CHECK_RETURNS
/* A callee that comes back somewhere other than the instruction after the call is a hand-made
 * return into the caller: continue at that address through its function, then leave this one. */
void recomp_continue(CPU *c, uint32_t expected);
#define RET_CHECK(a) do { if (__builtin_expect(c->r[15] != (a), 0)) { recomp_continue(c, (a)); return; } } while (0)
#else
#define RET_CHECK(a) ((void)0)
#endif

/* ---- memory ---- */
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
static inline uint32_t ld8(uint32_t a) { return *(uint8_t *)(uintptr_t)a; }
static inline void st32(uint32_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void st16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy((void *)(uintptr_t)a, &h, 2); }
static inline void st8(uint32_t a, uint32_t v) { *(uint8_t *)(uintptr_t)a = (uint8_t)v; }

/* ---- integer helpers ---- */
static inline uint32_t add_f(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t r = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)r;
    c->n = res >> 31; c->z = (res == 0); c->cf = (uint32_t)(r >> 32) & 1;
    c->v = ((~(a ^ b)) & (a ^ res)) >> 31;
    return res;
}
static inline uint32_t sh_lsl(uint32_t x, uint32_t s) { return s < 32 ? x << s : 0; }
static inline uint32_t sh_lsr(uint32_t x, uint32_t s) { return s < 32 ? x >> s : 0; }
static inline uint32_t sh_asr(uint32_t x, uint32_t s) { return (uint32_t)((int32_t)x >> (s < 32 ? s : 31)); }
static inline uint32_t sh_ror(uint32_t x, uint32_t s) { s &= 31; return s ? (x >> s) | (x << (32 - s)) : x; }
static inline uint32_t sh_lsl_c(uint32_t x, uint32_t s, uint32_t *co) {
    if (s == 0) return x;
    if (s < 32) { *co = (x >> (32 - s)) & 1; return x << s; }
    *co = (s == 32) ? (x & 1) : 0; return 0;
}
static inline uint32_t sh_lsr_c(uint32_t x, uint32_t s, uint32_t *co) {
    if (s == 0) return x;
    if (s < 32) { *co = (x >> (s - 1)) & 1; return x >> s; }
    *co = (s == 32) ? (x >> 31) : 0; return 0;
}
static inline uint32_t sh_asr_c(uint32_t x, uint32_t s, uint32_t *co) {
    if (s == 0) return x;
    if (s < 32) { *co = (x >> (s - 1)) & 1; return (uint32_t)((int32_t)x >> s); }
    *co = x >> 31; return (uint32_t)((int32_t)x >> 31);
}
static inline uint32_t sh_ror_c(uint32_t x, uint32_t s, uint32_t *co) {
    if (s == 0) return x;
    s &= 31;
    if (s == 0) { *co = x >> 31; return x; }
    *co = (x >> (s - 1)) & 1; return (x >> s) | (x << (32 - s));
}
static inline uint32_t uqsub8(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i += 8) {
        uint32_t a = (x >> i) & 0xff, b = (y >> i) & 0xff;
        r |= (a > b ? a - b : 0) << i;
    }
    return r;
}
static inline uint32_t uadd8_ge(CPU *c, uint32_t x, uint32_t y) {
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < 4; i++) { uint32_t s = ((x >> (8 * i)) & 0xff) + ((y >> (8 * i)) & 0xff); r |= (s & 0xff) << (8 * i); if (s >= 0x100) ge |= 1u << i; }
    c->ge = ge; return r;
}
static inline uint32_t sub8_ge(CPU *c, uint32_t x, uint32_t y, int is_signed) {
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < 4; i++) {
        int a = (int)((x >> (8 * i)) & 0xff), b = (int)((y >> (8 * i)) & 0xff);
        if (is_signed) { a = (int8_t)a; b = (int8_t)b; }
        int d = a - b;
        r |= ((uint32_t)d & 0xff) << (8 * i);
        if (d >= 0) ge |= 1u << i;
    }
    c->ge = ge; return r;
}
static inline uint32_t sel8(CPU *c, uint32_t n, uint32_t m) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= (((c->ge >> i) & 1) ? n : m) & (0xffu << (8 * i));
    return r;
}
static inline uint32_t uqadd8(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i += 8) {
        uint32_t t = ((x >> i) & 0xff) + ((y >> i) & 0xff);
        r |= (t > 255 ? 255 : t) << i;
    }
    return r;
}
static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
static inline uint32_t sat_s64(CPU *c, int64_t v) {
    if (v > 2147483647LL) { c->q = 1; return 0x7fffffffu; }
    if (v < -2147483648LL) { c->q = 1; return 0x80000000u; }
    return (uint32_t)(int32_t)v;
}
static inline uint32_t ssat_n(CPU *c, int32_t x, uint32_t bits) { /* saturate to signed 'bits' width */
    int32_t hi = (int32_t)((1u << (bits - 1)) - 1), lo = -hi - 1;
    if (x > hi) { c->q = 1; return (uint32_t)hi; }
    if (x < lo) { c->q = 1; return (uint32_t)lo; }
    return (uint32_t)x;
}
static inline uint32_t usat_n(CPU *c, int32_t x, uint32_t bits) { /* saturate to unsigned 'bits' width */
    int64_t hi = ((int64_t)1 << bits) - 1;
    if (x > hi) { c->q = 1; return (uint32_t)hi; }
    if (x < 0) { c->q = 1; return 0; }
    return (uint32_t)x;
}
static inline uint32_t get_cpsr(CPU *c) {
    return (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->v << 28) | (c->q << 27) | 0x10;
}
static inline void set_cpsr_f(CPU *c, uint32_t x) {
    c->n = x >> 31; c->z = (x >> 30) & 1; c->cf = (x >> 29) & 1; c->v = (x >> 28) & 1; c->q = (x >> 27) & 1;
}

/* ---- VFP helpers ---- */
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
#define GS(i) u2f(c->s[i])
#define SS(i, x) (c->s[i] = f2u(x))
static inline double getd(CPU *c, int i) { return u2d(c->s[2 * i] | ((uint64_t)c->s[2 * i + 1] << 32)); }
static inline void setd(CPU *c, int i, double d) { uint64_t u = d2u(d); c->s[2 * i] = (uint32_t)u; c->s[2 * i + 1] = (uint32_t)(u >> 32); }
#define GD(i) getd(c, i)
#define SD(i, x) setd(c, i, x)

#define VFP_NANCHK(r, fix) if (__builtin_expect((r) != (r), 0)) (r) = (fix)
static inline float vaddf(float a, float b) { float r = a + b; VFP_NANCHK(r, vfp_nan2f(a, b)); return r; }
static inline float vsubf(float a, float b) { float r = a - b; VFP_NANCHK(r, vfp_nan2f(a, b)); return r; }
static inline float vmulf(float a, float b) { float r = a * b; VFP_NANCHK(r, vfp_nan2f(a, b)); return r; }
static inline float vdivf(float a, float b) { float r = a / b; VFP_NANCHK(r, vfp_nan2f(a, b)); return r; }
static inline float vsqrtf(float a) { float r = sqrtf(a); VFP_NANCHK(r, vfp_nan1f(a)); return r; }
static inline float vnegf(float a) { return u2f(f2u(a) ^ 0x80000000u); }
static inline float vabsf(float a) { return u2f(f2u(a) & 0x7fffffffu); }
static inline double vaddd(double a, double b) { double r = a + b; VFP_NANCHK(r, vfp_nan2d(a, b)); return r; }
static inline double vsubd(double a, double b) { double r = a - b; VFP_NANCHK(r, vfp_nan2d(a, b)); return r; }
static inline double vmuld(double a, double b) { double r = a * b; VFP_NANCHK(r, vfp_nan2d(a, b)); return r; }
static inline double vdivd(double a, double b) { double r = a / b; VFP_NANCHK(r, vfp_nan2d(a, b)); return r; }
static inline double vsqrtd(double a) { double r = sqrt(a); VFP_NANCHK(r, vfp_nan1d(a)); return r; }
static inline double vnegd(double a) { return u2d(d2u(a) ^ 0x8000000000000000ull); }
static inline double vabsd(double a) { return u2d(d2u(a) & 0x7fffffffffffffffull); }
static inline float vcvt_d2f(double a) { float r = (float)a; VFP_NANCHK(r, u2f(f2u(r) | 0x00400000u)); return r; }
static inline double vcvt_f2d(float a) { double r = (double)a; VFP_NANCHK(r, u2d(d2u(r) | 0x0008000000000000ull)); return r; }
static inline void vcmp_set(CPU *c, int lt, int eq, int gt) {
    uint32_t f = eq ? 0x6u : lt ? 0x8u : gt ? 0x2u : 0x3u;
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}
#define VCMPF(a, b) do { float a_ = (a), b_ = (b); vcmp_set(c, a_ < b_, a_ == b_, a_ > b_); } while (0)
#define VCMPD(a, b) do { double a_ = (a), b_ = (b); vcmp_set(c, a_ < b_, a_ == b_, a_ > b_); } while (0)
static inline uint32_t vfp_d2si(double x) {
    if (x != x) return 0;
    if (x >= 2147483648.0) return 0x7fffffffu;
    if (x <= -2147483649.0) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}
static inline uint32_t vfp_d2ui(double x) {
    if (x != x) return 0;
    if (x >= 4294967296.0) return 0xffffffffu;
    if (x <= -1.0) return 0;
    return (uint32_t)x;
}
static inline void vfp_fmstat(CPU *c) {
    uint32_t f = c->fpscr;
    c->n = f >> 31; c->z = (f >> 30) & 1; c->cf = (f >> 29) & 1; c->v = (f >> 28) & 1;
}
