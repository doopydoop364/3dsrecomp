/* PICA200 vertex shader interpreter. Computes in float32 (as the reference does); see docs/pica_facts.md. */
#include "pica.h"
#include <math.h>

static int warned_unimpl[64];

static float fmul(float a, float b) {          /* 0 * inf = 0 on the PICA */
    float r = a * b;
    if (r != r && a == a && b == b) return 0.0f;
    return r;
}

typedef struct {
    float r[16][4];
    int32_t a0x, a0y, aL;
    int cc[2];
} ShState;

static const float ONES[4] = { 1, 1, 1, 1 };

/* Fetch a source register with swizzle and negation. raw: 5 or 7-bit register index;
 * rel: address register selector (0 none, 1 a0.x, 2 a0.y, 3 aL), only applied to uniforms. */
static void fetch(const PicaShader *sh, const ShState *st, const float in[16][4], uint32_t raw, uint32_t rel,
                  uint32_t swz, int neg, float out[4]) {
    const float *src;
    if (raw < 0x10) src = in[raw];
    else if (raw < 0x20) src = st->r[raw - 0x10];
    else {
        int32_t u = (int32_t)raw - 0x20;
        if (rel) {
            int32_t off = rel == 1 ? st->a0x : rel == 2 ? st->a0y : st->aL;
            if (off < -128 || off > 127) off = 0;
            u = (u + off) & 0x7f;
        }
        src = u < 96 ? sh->uniform[u] : ONES;
    }
    for (int i = 0; i < 4; i++) {
        float v = src[(swz >> (6 - 2 * i)) & 3];
        out[i] = neg ? -v : v;
    }
}
static void store(float dst_o[16][4], ShState *st, uint32_t dest, uint32_t mask, const float v[4]) {
    float *d;
    if (dest < 0x10) d = dst_o[dest];
    else if (dest < 0x20) d = st->r[dest - 0x10];
    else return;
    for (int i = 0; i < 4; i++) if (mask & (8 >> i)) d[i] = v[i];
}
static int cmp_op(uint32_t op, float a, float b) {
    switch (op) {
    case 0: return a == b; case 1: return a != b; case 2: return a < b;
    case 3: return a <= b; case 4: return a > b; case 5: return a >= b;
    default: return 0;
    }
}
static int sh_test = -1;                       /* RB_SH_TEST: experimental variants for the replay test */
static int cond(const ShState *st, uint32_t w) {
    if (sh_test < 0) { const char *e = getenv("RB_SH_TEST"); sh_test = e ? atoi(e) : 0; }
    int rx = ((w >> 25) & 1) == (uint32_t)st->cc[0], ry = ((w >> 24) & 1) == (uint32_t)st->cc[1];
    if (sh_test == 2) { rx = ((w >> 24) & 1) == (uint32_t)st->cc[0]; ry = ((w >> 25) & 1) == (uint32_t)st->cc[1]; }
    if (sh_test == 1) { int r; switch ((w >> 22) & 3) { case 0: r = rx || ry; break; case 1: r = rx && ry; break; case 2: r = rx; break; default: r = ry; } return !r; }
    switch ((w >> 22) & 3) {
    case 0: return rx || ry;
    case 1: return rx && ry;
    case 2: return rx;
    default: return ry;
    }
}

typedef struct { uint32_t kind, end, ret, start, count, inc; int32_t saved_aL; } Frame;   /* kind: 1 call, 2 if, 3 loop */

/* Runs the program from entry. Returns 0 on success, -1 on a runaway program. */
int pica_shader_run(const PicaShader *sh, uint32_t entry, const float in[16][4], float out[16][4]) {
    ShState st;
    memset(&st, 0, sizeof st);
    Frame stack[16]; int sp = 0;
    uint32_t pc = entry;
    for (int steps = 0; steps < 200000; steps++) {
        if (pc >= 512) return 0;
        uint32_t w = sh->code[pc], op = w >> 26, next = pc + 1;
        int jumped = 0;
        if (op < 0x20) {                                              /* arithmetic, common format */
            int inv = op >= 0x18 && op <= 0x1b;
            uint32_t d = sh->desc[w & 0x7f], dest = (w >> 21) & 0x1f, mask = d & 15, rel = (w >> 19) & 3;
            uint32_t s1 = inv ? (w >> 14) & 0x1f : (w >> 12) & 0x7f, s2 = inv ? (w >> 7) & 0x7f : (w >> 7) & 0x1f;
            float a[4], b[4], r[4];
            fetch(sh, &st, in, s1, inv ? 0 : rel, (d >> 5) & 0xff, (d >> 4) & 1, a);
            fetch(sh, &st, in, s2, inv ? rel : 0, (d >> 14) & 0xff, (d >> 13) & 1, b);
            switch (op) {
            case 0x00: for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break;
            case 0x01: { float s = fmul(a[0], b[0]) + fmul(a[1], b[1]) + fmul(a[2], b[2]); r[0] = r[1] = r[2] = r[3] = s; break; }
            case 0x02: { float s = fmul(a[0], b[0]) + fmul(a[1], b[1]) + fmul(a[2], b[2]) + fmul(a[3], b[3]); r[0] = r[1] = r[2] = r[3] = s; break; }
            case 0x03: case 0x18: { float s = fmul(a[0], b[0]) + fmul(a[1], b[1]) + fmul(a[2], b[2]) + b[3]; r[0] = r[1] = r[2] = r[3] = s; break; }
            case 0x05: r[0] = r[1] = r[2] = r[3] = exp2f(a[0]); break;
            case 0x06: r[0] = r[1] = r[2] = r[3] = log2f(a[0]); break;
            case 0x08: for (int i = 0; i < 4; i++) r[i] = fmul(a[i], b[i]); break;
            case 0x09: case 0x1a: for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
            case 0x0a: case 0x1b: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
            case 0x0b: for (int i = 0; i < 4; i++) r[i] = floorf(a[i]); break;
            case 0x0c: for (int i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i]; break;
            case 0x0d: for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case 0x0e: r[0] = r[1] = r[2] = r[3] = 1.0f / a[0]; break;
            case 0x0f: r[0] = r[1] = r[2] = r[3] = 1.0f / sqrtf(a[0]); break;
            case 0x12: if (mask & 8) st.a0x = (int32_t)a[0]; if (mask & 4) st.a0y = (int32_t)a[1]; goto done;   /* MOVA */
            case 0x13: memcpy(r, a, sizeof r); break;
            default:
                if (!warned_unimpl[op]) { warned_unimpl[op] = 1; fprintf(stderr, "pica: shader opcode %02x is not implemented (result left unchanged)\n", op); }
                goto done;
            }
            store(out, &st, dest, mask, r);
        } else if ((op & ~1u) == 0x2e) {                             /* CMP */
            uint32_t d = sh->desc[w & 0x7f], rel = (w >> 19) & 3;
            float a[4], b[4];
            fetch(sh, &st, in, (w >> 12) & 0x7f, rel, (d >> 5) & 0xff, (d >> 4) & 1, a);
            fetch(sh, &st, in, (w >> 7) & 0x1f, 0, (d >> 14) & 0xff, (d >> 13) & 1, b);
            uint32_t ox = (w >> 24) & 7, oy = (w >> 21) & 7;
            if (sh_test == 3) { uint32_t t = ox; ox = oy; oy = t; }
            if (ox < 6) st.cc[0] = cmp_op(ox, a[0], b[0]);
            if (oy < 6) st.cc[1] = cmp_op(oy, a[1], b[1]);
        } else if (op >= 0x30) {                                      /* MAD / MADI */
            int madi = (op & ~7u) == 0x30;
            uint32_t d = sh->desc[w & 0x1f], dest = (w >> 24) & 0x1f, rel = (w >> 22) & 3;
            uint32_t s1 = (w >> 17) & 0x1f, s2 = madi ? (w >> 12) & 0x1f : (w >> 10) & 0x7f, s3 = madi ? (w >> 5) & 0x7f : (w >> 5) & 0x1f;
            float a[4], b[4], c3[4], r[4];
            fetch(sh, &st, in, s1, 0, (d >> 5) & 0xff, (d >> 4) & 1, a);
            fetch(sh, &st, in, s2, madi ? 0 : rel, (d >> 14) & 0xff, (d >> 13) & 1, b);
            fetch(sh, &st, in, s3, madi ? rel : 0, (d >> 23) & 0xff, (d >> 22) & 1, c3);
            for (int i = 0; i < 4; i++) r[i] = fmul(a[i], b[i]) + c3[i];
            store(out, &st, dest, d & 15, r);
        } else {                                                      /* flow control */
            uint32_t target = (w >> 10) & 0xfff, count = w & 0xff;
            int bval = (sh->bools >> ((w >> 22) & 15)) & 1;
            if (sh_test < 0) { const char *e = getenv("RB_SH_TEST"); sh_test = e ? atoi(e) : 0; }
            if (sh_test == 4 && op == 0x27) bval = !bval;                 /* test: invert IFU */
            if (sh_test == 5) bval = (sh->bools >> (15 - ((w >> 22) & 15))) & 1;   /* test: reversed bool bit order */
            if (sh_test == 6 && (op == 0x26 || op == 0x2d)) bval = !bval;  /* test: invert CALLU/JMPU */
            switch (op) {
            case 0x20: case 0x23:                                     /* BREAK, BREAKC */
                if (op == 0x23 && !cond(&st, w)) break;
                for (int k = sp - 1; k >= 0; k--)
                    if (stack[k].kind == 3) { next = stack[k].end + 1; st.aL += (int32_t)stack[k].inc; sp = k; jumped = 1; break; }
                break;
            case 0x21: break;
            case 0x22: return 0;                                      /* END */
            case 0x24: case 0x25: case 0x26:                          /* CALL, CALLC, CALLU */
                if ((op == 0x25 && !cond(&st, w)) || (op == 0x26 && !bval)) break;
                if (sp < 16) stack[sp++] = (Frame){ 1, target + count, pc + 1, 0, 0, 0, 0 };
                next = target; jumped = 1; break;
            case 0x27: case 0x28: {                                   /* IFU, IFC */
                int t = op == 0x27 ? bval : cond(&st, w);
                if (t) { if (sp < 16) stack[sp++] = (Frame){ 2, target, target + count, 0, 0, 0, 0 }; }
                else { next = target; jumped = 1; }
                break;
            }
            case 0x29: {                                              /* LOOP */
                uint32_t iu = sh->ints[(w >> 22) & 3];
                if (sp < 16) stack[sp++] = (Frame){ 3, target, 0, pc + 1, iu & 0xff, (uint32_t)(int8_t)(iu >> 16), st.aL };
                st.aL = (int32_t)((iu >> 8) & 0xff);
                break;
            }
            case 0x2a: case 0x2b: break;                              /* EMIT / SETEMIT: geometry shader only */
            case 0x2c: if (cond(&st, w)) { next = target; jumped = 1; } break;
            case 0x2d: if (bval == !(count & 1)) { next = target; jumped = 1; } break;
            default:
                if (!warned_unimpl[op]) { warned_unimpl[op] = 1; fprintf(stderr, "pica: shader opcode %02x is not implemented\n", op); }
            }
        }
    done:
        /* end of an if body, call body or loop body reached */
        while (sp > 0 && !jumped) {
            Frame *f = &stack[sp - 1];
            if (f->kind == 1 && pc == f->end - 1) { next = f->ret; sp--; continue; }
            if (f->kind == 2 && pc == f->end - 1) { next = f->ret; sp--; continue; }   /* if body done: skip else */
            if (f->kind == 3 && pc == f->end) {
                st.aL += (int32_t)f->inc;
                if (f->count--) next = f->start;
                else { if (sp > 1) st.aL = f->saved_aL; sp--; }
            }
            break;
        }
        pc = next;
    }
    return -1;
}
