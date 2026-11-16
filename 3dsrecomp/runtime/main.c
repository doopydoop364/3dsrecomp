/* Loader: builds the guest address space and starts the game's main thread. */
#define _GNU_SOURCE
#include "audio.h"
#include "host.h"
#include "input.h"
#include "settings.h"
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern const struct { uint32_t addr; recomp_fn fn; } recomp_functions[];
extern const uint32_t recomp_function_count;
recomp_fn *recomp_table;

void video_init(const char *title);
void gpu_report(void);
static void on_stop(int sig) { (void)sig; gpu_report(); _exit(0); }
static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    fatal("host signal %d at guest/host address %p (a guest access to unmapped memory looks like this)", sig, si->si_addr);
}
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *prog_name = "game";
static void usage(void) {
    fprintf(stderr, "usage: %s [game folder] [options]\n"
        "  --scale N            internal resolution 1-4          --layout NAME   stacked, large_top, top_only, side_by_side\n"
        "  --renderer NAME      vulkan or software               --filter NAME   console, nearest, linear\n"
        "  --fullscreen         start in fullscreen              --set SECTION.KEY=VALUE   any settings.toml value\n", prog_name);
    exit(1);
}
int main(int argc, char **argv) {
    const char *dir = "game";
    prog_name = argv[0];
    static const char *cli[64][2]; int ncli = 0;
    for (int i = 1; i < argc; i++) {                     /* flags are applied after the settings files are read */
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (a[0] != '-') { dir = a; continue; }
        if (ncli >= 63) break;
        if (!strcmp(a, "--fullscreen")) { cli[ncli][0] = "video.fullscreen"; cli[ncli++][1] = "true"; }
        else if (!v) usage();
        else if (!strcmp(a, "--scale")) { cli[ncli][0] = "video.scale"; cli[ncli++][1] = v; i++; }
        else if (!strcmp(a, "--layout")) { cli[ncli][0] = "video.layout"; cli[ncli++][1] = v; i++; }
        else if (!strcmp(a, "--renderer")) { cli[ncli][0] = "video.renderer"; cli[ncli++][1] = v; i++; }
        else if (!strcmp(a, "--filter")) { cli[ncli][0] = "video.texture_filter"; cli[ncli++][1] = v; i++; }
        else if (!strcmp(a, "--set") && strchr(v, '=')) {
            static char kv[64][128]; snprintf(kv[ncli], sizeof kv[ncli], "%s", v);
            char *e = strchr(kv[ncli], '='); *e = 0; cli[ncli][0] = kv[ncli]; cli[ncli++][1] = e + 1; i++;
        } else usage();
    }
    host_game_dir = dir;
    if (getenv("RB_LOG")) log_level = atoi(getenv("RB_LOG"));
    char path[512]; uint8_t exh[0x800];
    snprintf(path, sizeof path, "%s/exheader.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f || fread(exh, 1, sizeof exh, f) != sizeof exh) fatal("cannot read %s", path);
    fclose(f);
    uint32_t text = rd32le(exh + 0x10), text_sz = rd32le(exh + 0x14) << 12, stack_sz = rd32le(exh + 0x1c);
    uint32_t ro = rd32le(exh + 0x20), ro_sz = rd32le(exh + 0x24) << 12;
    uint32_t data = rd32le(exh + 0x30), data_sz = rd32le(exh + 0x34) << 12, bss_sz = rd32le(exh + 0x3c);
    uint64_t title = (uint64_t)rd32le(exh + 0x200) | (uint64_t)rd32le(exh + 0x204) << 32;
    if (title != RECOMP_TITLE_ID || text != RECOMP_TEXT_BASE || text_sz != RECOMP_TEXT_SIZE || data != RECOMP_RO_END)
        fatal("this executable was generated for title %016llx but %s/exheader.bin is for title %016llx (or has another layout); regenerate with build.sh",
              (unsigned long long)RECOMP_TITLE_ID, dir, (unsigned long long)title);

    uint8_t *p_text = mem_map(text, text_sz, ".text", 4, 5);
    uint8_t *p_ro = mem_map(ro, ro_sz, ".rodata", 4, 1);
    uint8_t *p_data = mem_map(data, data_sz + bss_sz, ".data/.bss", 5, 3);
    snprintf(path, sizeof path, "%s/code_decompressed.bin", dir);
    f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    if (fread(p_text, 1, text_sz, f) != text_sz || fread(p_ro, 1, ro_sz, f) != ro_sz || fread(p_data, 1, data_sz, f) != data_sz)
        fatal("%s is shorter than the ExHeader says", path);
    fclose(f);
    mprotect(p_text, text_sz, PROT_READ); mprotect(p_ro, ro_sz, PROT_READ);

    mem_map(STACK_TOP - stack_sz, stack_sz, "main stack", 11, 3);
    mem_map(VRAM_BASE, VRAM_SIZE, "VRAM", 6, 3);
    mem_map(0x1ff00000u, 0x80000, "DSP memory", 6, 3);
    uint8_t *cfg = mem_map(CONFIG_PAGE, 0x1000, "config page", 6, 1);
    uint8_t *shp = mem_map(SHARED_PAGE, 0x1000, "shared page", 6, 1);
    mem_map(TLS_BASE, TLS_SIZE * MAX_THREADS, "thread local storage", 5, 3);

    /* kernel configuration page: firmware 2.50, retail unit, 64 MB application memory */
    cfg[0x02] = 50; cfg[0x03] = 2; cfg[0x14] = 1; cfg[0x10] = 2;
    *(uint32_t *)(cfg + 0x40) = 0x04000000; *(uint32_t *)(cfg + 0x44) = 0x02c00000; *(uint32_t *)(cfg + 0x48) = 0x01400000;
    cfg[0x62] = 50; cfg[0x63] = 2; cfg[0x64] = 2;
    /* shared page: clock, retail hardware, 3D slider off, full battery */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms1900 = ((uint64_t)ts.tv_sec + 2208988800ull) * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    unsigned __int128 tick = (unsigned __int128)now_ns() * 268111856u / 1000000000u;
    shp[0x04] = 1;
    *(uint64_t *)(shp + 0x20) = ms1900; *(uint64_t *)(shp + 0x28) = (uint64_t)tick; *(uint64_t *)(shp + 0x30) = 0xffb0ff0;
    memcpy(shp + 0x40, shp + 0x20, 0x20);
    *(float *)(shp + 0x80) = 0.0f; shp[0x84] = 1; shp[0x85] = (5 << 2) | 1;

    recomp_table = calloc(RECOMP_TEXT_SIZE / 4, sizeof(recomp_fn));
    for (uint32_t i = 0; i < recomp_function_count; i++)
        recomp_table[(recomp_functions[i].addr - RECOMP_TEXT_BASE) >> 2] = recomp_functions[i].fn;

    stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    sigaltstack(&ss, 0);
    struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK };
    signal(SIGTERM, on_stop); signal(SIGINT, on_stop);
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0); sigaction(SIGILL, &sa, 0);

    settings_load(title);
    for (int i = 0; i < ncli; i++) settings_set(cli[i][0], cli[i][1]);
    if (!getenv("RB_SCALE")) setenv("RB_SCALE", settings_get("video.scale", "1"), 1);
    if (!getenv("RB_ANISOTROPY")) setenv("RB_ANISOTROPY", settings_get("video.anisotropy", "1"), 1);
    if (!getenv("RB_TEX_FILTER")) setenv("RB_TEX_FILTER", settings_get("video.texture_filter", "console"), 1);
    if (!getenv("RB_RENDERER")) setenv("RB_RENDERER", settings_get("video.renderer", "vulkan"), 1);   /* vk.c reads it */
    video_init("3dsrecomp");
    input_init();
    audio_init();
    services_init(dir);
    k_thread_create(text, 0, STACK_TOP, exh[0x20f] ? exh[0x20f] : 0x30);
    k_run();
}
