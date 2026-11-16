/* VFP corner cases that differ between ARM and x86: NaN selection and FPSCR control bits. */
#include "recomp.h"
#include <fenv.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

static __thread int default_nan;   /* FPSCR.DN */

static int is_nan_f(uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }
static int is_snan_f(uint32_t u) { return is_nan_f(u) && !(u & 0x00400000u); }
static int is_nan_d(uint64_t u) { return (u & 0x7fffffffffffffffull) > 0x7ff0000000000000ull; }
static int is_snan_d(uint64_t u) { return is_nan_d(u) && !(u & 0x0008000000000000ull); }

float vfp_nan2f(float a, float b) {
    uint32_t x = f2u(a), y = f2u(b);
    if (!default_nan) {
        if (is_snan_f(x)) return u2f(x | 0x00400000u);
        if (is_snan_f(y)) return u2f(y | 0x00400000u);
        if (is_nan_f(x)) return a;
        if (is_nan_f(y)) return b;
    }
    return u2f(0x7fc00000u);
}
float vfp_nan1f(float a) {
    uint32_t x = f2u(a);
    if (!default_nan && is_nan_f(x)) return u2f(x | 0x00400000u);
    return u2f(0x7fc00000u);
}
double vfp_nan2d(double a, double b) {
    uint64_t x = d2u(a), y = d2u(b);
    if (!default_nan) {
        if (is_snan_d(x)) return u2d(x | 0x0008000000000000ull);
        if (is_snan_d(y)) return u2d(y | 0x0008000000000000ull);
        if (is_nan_d(x)) return a;
        if (is_nan_d(y)) return b;
    }
    return u2d(0x7ff8000000000000ull);
}
double vfp_nan1d(double a) {
    uint64_t x = d2u(a);
    if (!default_nan && is_nan_d(x)) return u2d(x | 0x0008000000000000ull);
    return u2d(0x7ff8000000000000ull);
}

void vfp_set_fpscr(CPU *c, uint32_t v) {
    static const int rmode[4] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
    if (v & 0x00370000u)
        recomp_trap(c, 0, v, "VFP short-vector mode is not supported");
    c->fpscr = v;
    fesetround(rmode[(v >> 22) & 3]);
    int fz = (v >> 24) & 1;
    _MM_SET_FLUSH_ZERO_MODE(fz ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(fz ? _MM_DENORMALS_ZERO_ON : _MM_DENORMALS_ZERO_OFF);
    default_nan = (v >> 25) & 1;
}
