/* GPU replay test: run one PICA200 command list through this port's renderer on memory captured from
 * a reference emulator, so the two renderers can be compared on identical input.
 *
 * Usage: gpu_replay <pica state> <vram before> <linear before> <list addr hex> <list size hex> <vram out>
 *   pica state     PICA registers/shader state saved by the port (RB_GX_LOG) before the same list
 *   vram/linear    memory dumps of 0x1f000000 (6 MB) and 0x30000000 (linear heap) taken before the list ran
 */
#define _GNU_SOURCE
#include "pica.h"
#include "vkr.h"
#include <sys/mman.h>
#include <time.h>

KThread *cur;
int log_level = 1;
recomp_fn *recomp_table;
uint64_t gpu_frame(void) { return 0; }
void fatal(const char *fmt, ...) { fprintf(stderr, "fatal: %s\n", fmt); exit(1); }
void recomp_trap(CPU *c, uint32_t addr, uint32_t info, const char *why) { (void)c; fprintf(stderr, "trap: %s %08x %08x\n", why, addr, info); exit(1); }

static struct { uint32_t base, size; } regions[4];
static int nregions;
int mem_valid(uint32_t addr, uint32_t size) {
    for (int i = 0; i < nregions; i++)
        if (addr - regions[i].base < regions[i].size) return size <= regions[i].size - (addr - regions[i].base);
    return 0;
}
static void load_region(uint32_t base, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t size = ((uint32_t)n + 0xfff) & ~0xfffu;
    void *p = mmap((void *)(uintptr_t)base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)base) { perror("mmap"); exit(1); }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(1); }
    fclose(f);
    regions[nregions].base = base; regions[nregions].size = size; nregions++;
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: gpu_replay state vram linear list size out\n"); return 1; }
    load_region(0x1f000000u, argv[2]);
    load_region(0x30000000u, argv[3]);
    if (pica_load_state(argv[1])) { fprintf(stderr, "cannot load PICA state %s\n", argv[1]); return 1; }
    /* list "-" and size <file>: run every list named in the file (one "addr size" per line), a whole frame */
    uint32_t lists[64][2]; int nl = 0;
    if (!strcmp(argv[4], "-")) {
        FILE *lf = fopen(argv[5], "r");
        if (!lf) { perror(argv[5]); return 1; }
        while (nl < 64 && fscanf(lf, "%x %x", &lists[nl][0], &lists[nl][1]) == 2) nl++;
        fclose(lf);
    } else { lists[0][0] = (uint32_t)strtoul(argv[4], 0, 16); lists[0][1] = (uint32_t)strtoul(argv[5], 0, 16); nl = 1; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int reps = getenv("RB_BENCH") ? atoi(getenv("RB_BENCH")) : 1;
    for (int r = 0; r < reps; r++)                   /* repeated runs re-render the same pixels: only for timing */
        for (int k = 0; k < nl; k++) pica_run_list(lists[k][0], lists[k][1]);
    vkr_sync_all();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "render time: %.2f ms per run\n", ((double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / reps);
    pica_report();
    FILE *o = fopen(argv[6], "wb");
    if (!o) { perror(argv[6]); return 1; }
    fwrite((void *)(uintptr_t)0x1f000000u, 1, 0x600000, o); fclose(o);
    if (argc > 7) {                                   /* optional: compare the render target with the reference result */
        FILE *r = fopen(argv[7], "rb"); static uint8_t ref[240 * 400 * 4];
        if (r && fread(ref, 1, sizeof ref, r) == sizeof ref) {
            const uint8_t *m = (const uint8_t *)(uintptr_t)0x1f000000u; int diff = 0;
            for (int i = 0; i < (int)sizeof ref; i += 4) diff += memcmp(m + i, ref + i, 4) != 0;
            printf("render target pixels differing from reference: %d\n", diff);
        }
        if (r) fclose(r);
    }
    return 0;
}
