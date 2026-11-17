/* Differential test runner: executes recompiled functions on prepared inputs and dumps the
 * resulting CPU and memory state, to be compared against a reference ARM emulator. */
#define _GNU_SOURCE
#include "recomp.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <zlib.h>

extern const struct { uint32_t addr; recomp_fn fn; } recomp_functions[];
extern const uint32_t recomp_function_count;
recomp_fn *recomp_table, *recomp_table_thumb;

#define IMG_BASE     RECOMP_TEXT_BASE     /* layout comes from the generated layout.h */
#define RO_END       RECOMP_RO_END
#define RW_END       RECOMP_RW_END
#define SCRATCH_BASE 0x08000000u
#define SCRATCH_SIZE 0x00010000u
#define STACK_BASE   0x0ffe0000u
#define STACK_SIZE   0x00010000u
#define RET_MAGIC    0x1ff00000u
#define NPAT 6

typedef struct { uint32_t fn, pattern, r[13], flags, s[32]; } TestIn;
typedef struct { uint32_t status, r[15], flags, s[32], fpscr, crc_scratch, crc_stack, crc_data; } TestOut;

static sigjmp_buf env;
static volatile int in_test;
static const char *trap_why = "";
static uint32_t trap_addr, trap_info;

uint32_t recomp_site;
void recomp_continue(CPU *c, uint32_t expected) { recomp_trap(c, expected, c->r[15], "callee returned to an unexpected address"); }
void recomp_trap(CPU *c, uint32_t addr, uint32_t info, const char *why) {
    (void)c; trap_why = why; trap_addr = addr; trap_info = info;
    siglongjmp(env, 1);
}
void call_indirect(CPU *c, uint32_t addr) {
    recomp_fn f = recomp_lookup(addr);
    if (!f) recomp_trap(c, addr, 0, "indirect call to unknown function");
    f(c);
}
int recomp_budget = PREEMPT_BUDGET;
void recomp_preempt(CPU *c) { (void)c; recomp_budget = PREEMPT_BUDGET; }
void svc_call(CPU *c, uint32_t num, uint32_t addr) { recomp_trap(c, addr, num, "svc"); }

static void on_signal(int sig) {
    if (!in_test) _exit(99);
    trap_why = sig == SIGALRM ? "timeout" : "host fault"; trap_addr = 0; trap_info = (uint32_t)sig;
    siglongjmp(env, sig == SIGALRM ? 3 : 2);
}

static void *map_fixed(uint32_t addr, uint32_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)addr) { perror("mmap fixed"); exit(1); }
    return p;
}
static void gen_pattern(uint32_t p, uint32_t *buf) {
    uint32_t x = 0x9e3779b9u * (p + 1), kind = p % 3;
    for (uint32_t i = 0; i < SCRATCH_SIZE / 4; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = kind == 0 ? x : kind == 1 ? SCRATCH_BASE + ((x % (SCRATCH_SIZE - 0x400)) & ~3u) : (x & 0xff);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: runner image vectors results\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    uint8_t *img = map_fixed(IMG_BASE, RW_END - IMG_BASE);
    size_t got = fread(img, 1, RW_END - IMG_BASE, f); fclose(f);
    static uint8_t pristine[RW_END - RO_END];
    memcpy(pristine, (void *)(uintptr_t)RO_END, sizeof pristine);
    mprotect(img, RO_END - IMG_BASE, PROT_READ);
    map_fixed(SCRATCH_BASE, SCRATCH_SIZE);
    map_fixed(STACK_BASE, STACK_SIZE);
    static uint32_t patterns[NPAT][SCRATCH_SIZE / 4];
    for (uint32_t p = 0; p < NPAT; p++) gen_pattern(p, patterns[p]);

    recomp_table_build(recomp_functions, recomp_function_count, calloc);

    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    sigaltstack(&ss, 0);
    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_ONSTACK | SA_NODEFER };
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGALRM };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);

    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (!in || !out) { perror("vectors/results"); return 1; }
    TestIn t; (void)got;
    while (fread(&t, sizeof t, 1, in) == 1) {
        static CPU cpu;
        TestOut o;
        memset(&cpu, 0, sizeof cpu); memset(&o, 0, sizeof o);
        memcpy((void *)(uintptr_t)RO_END, pristine, sizeof pristine);
        memcpy((void *)(uintptr_t)SCRATCH_BASE, patterns[t.pattern % NPAT], SCRATCH_SIZE);
        memset((void *)(uintptr_t)STACK_BASE, 0, STACK_SIZE);
        memcpy(cpu.r, t.r, sizeof t.r);
        cpu.r[13] = STACK_BASE + STACK_SIZE - 0x1000; cpu.r[14] = RET_MAGIC;
        cpu.n = (t.flags >> 3) & 1; cpu.z = (t.flags >> 2) & 1; cpu.cf = (t.flags >> 1) & 1; cpu.v = t.flags & 1;
        memcpy(cpu.s, t.s, sizeof t.s);
        cpu.tls = SCRATCH_BASE + 0x8000;
        vfp_set_fpscr(&cpu, 0);
        recomp_fn fn = recomp_lookup(t.fn);
        int st = sigsetjmp(env, 1);
        if (st == 0) {
            if (!fn) recomp_trap(&cpu, t.fn, 0, "function not recompiled");
            struct itimerval tv = { {0, 0}, {0, 250000} };
            in_test = 1; setitimer(ITIMER_REAL, &tv, 0);
            fn(&cpu);
            if (cpu.r[15] != RET_MAGIC) recomp_trap(&cpu, t.fn, cpu.r[15], "returned to an unexpected address");
        }
        struct itimerval off = { {0, 0}, {0, 0} };
        setitimer(ITIMER_REAL, &off, 0); in_test = 0;
        o.status = (uint32_t)st;
        if (st) fprintf(stderr, "%08x %d %s @%08x info=%08x\n", t.fn, st, trap_why, trap_addr, trap_info);
        memcpy(o.r, cpu.r, sizeof o.r);
        o.flags = (cpu.n << 3) | (cpu.z << 2) | (cpu.cf << 1) | cpu.v;
        memcpy(o.s, cpu.s, sizeof o.s);
        o.fpscr = cpu.fpscr;
        o.crc_scratch = (uint32_t)crc32(0, (void *)(uintptr_t)SCRATCH_BASE, SCRATCH_SIZE);
        o.crc_stack = (uint32_t)crc32(0, (void *)(uintptr_t)STACK_BASE, STACK_SIZE);
        o.crc_data = (uint32_t)crc32(0, (void *)(uintptr_t)RO_END, RW_END - RO_END);
        fwrite(&o, sizeof o, 1, out);
    }
    fclose(out);
    return 0;
}
