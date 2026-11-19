/* Save states: the 3DS's own state (guest memory, threads, kernel objects, services, GPU and DSP registers), written
 * by each part of the runtime through one visitor (xxx_state(SS *)), which saves or loads the same fields in the same
 * order. Nothing host-specific goes in: no host addresses, no file descriptors, no function pointers. Threads continue
 * from their registers (kernel.c, recomp resume points), open files are reopened by path, sessions find their
 * handler by name. So a state survives rebuilding the game, and loads in a fresh process.
 *
 * A state is taken at a vertical blank, when no guest thread is on the host stack (gpu.c). Loading restarts the
 * program with RB_LOAD_STATE=<file> (main.c), which sets up as usual and then replaces the 3DS state.
 * File: gzip; "3DSRSTAT", format version, title id, frame, then the sections in a fixed order, each with its tag. */
#include "host.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <zlib.h>
#include "vkr.h"

#define STATE_VERSION 1

struct SS { gzFile g; int load, failed; const char *path; };

int ss_loading(SS *s) { return s->load; }
void ss_raw(SS *s, void *p, size_t n) {
    if (s->failed || !n) return;
    int r = s->load ? gzread(s->g, p, (unsigned)n) : gzwrite(s->g, p, (unsigned)n);
    if (r != (int)n) { s->failed = 1; fatal("save state %s: %s failed (%s)", s->path, s->load ? "reading" : "writing", s->load ? "the file is cut short" : strerror(errno)); }
}
void ss_big(SS *s, void *p, size_t n) {                     /* large blocks (guest memory) in pieces */
    uint8_t *b = p;
    while (n) { size_t k = n > (1u << 30) ? 1u << 30 : n; ss_raw(s, b, k); b += k; n -= k; }
}
void ss_tag(SS *s, const char *tag) {
    char t[16] = { 0 }; snprintf(t, sizeof t, "%s", tag);
    char got[16]; memcpy(got, t, 16);
    ss_raw(s, got, 16);
    if (s->load && memcmp(got, t, 16)) fatal("save state %s does not match this program: expected section \"%s\", found \"%.16s\"", s->path, tag, got);
}
void ss_str(SS *s, char *buf, size_t n) {
    uint32_t len = s->load ? 0 : (uint32_t)strnlen(buf, n);
    SS_V(s, len);
    if (s->load && len >= n) fatal("save state %s: a string does not fit", s->path);
    ss_raw(s, buf, len);
    if (s->load) buf[len] = 0;
}

void kernel_state(SS *s); void services_state(SS *s); void gpu_state(SS *s); void pica_state(SS *s);
void dsp_state(SS *s); void y2r_state(SS *s); void nfc_state(SS *s); void ldr_ro_state(SS *s); void modules_state(SS *s);
void vkr_forget(void); void audio_flush(void); void kernel_after_load(void);

static void visit(SS *s) {
    kernel_state(s);                     /* first: memory, objects and threads the others refer to */
    modules_state(s);
    ldr_ro_state(s);
    services_state(s);
    gpu_state(s);
    pica_state(s);
    dsp_state(s);
    y2r_state(s);
    nfc_state(s);
    ss_tag(s, "end");
}

/* Write a state now (called at a vertical blank). Returns 0 on success. */
int state_save(const char *path) {
    vkr_sync_all();                                      /* what the GPU drew goes to guest memory first */
    { const char *d = getenv("RB_MEMDUMP_SAVE"); if (d) mem_dump_all(d); }   /* test: guest memory as saved */
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.part", path);
    gzFile g = gzopen(tmp, "wb1");
    if (!g) { fprintf(stderr, "state: cannot write %s (%s)\n", tmp, strerror(errno)); return -1; }
    SS s = { g, 0, 0, path };
    char magic[8] = "3DSRSTAT"; uint32_t ver = STATE_VERSION; uint64_t tid = RECOMP_TITLE_ID, frame = gpu_frame();
    ss_raw(&s, magic, 8); SS_V(&s, ver); SS_V(&s, tid); SS_V(&s, frame);
    visit(&s);
    if (gzclose(g) != Z_OK || rename(tmp, path)) { fprintf(stderr, "state: writing %s failed\n", path); unlink(tmp); return -1; }
    fprintf(stderr, "state: saved %s at frame %llu\n", path, (unsigned long long)frame);
    return 0;
}

/* Replace the 3DS state with a saved one: called by main once everything is set up, before any guest code runs. */
void state_load(const char *path) {
    gzFile g = gzopen(path, "rb");
    if (!g) fatal("cannot open save state %s (%s)", path, strerror(errno));
    SS s = { g, 1, 0, path };
    char magic[8]; uint32_t ver; uint64_t tid, frame;
    ss_raw(&s, magic, 8); SS_V(&s, ver); SS_V(&s, tid); SS_V(&s, frame);
    if (memcmp(magic, "3DSRSTAT", 8)) fatal("%s is not a save state", path);
    if (ver != STATE_VERSION) fatal("save state %s has format %u; this program reads format %u", path, ver, STATE_VERSION);
    if (tid != RECOMP_TITLE_ID) fatal("save state %s is for title %016llx, not this game (%016llx)", path, (unsigned long long)tid, (unsigned long long)RECOMP_TITLE_ID);
    visit(&s);
    gzclose(g);
    vkr_forget();                                        /* the GPU caches start over from guest memory */
    audio_flush();
    kernel_after_load();
    fprintf(stderr, "state: loaded %s (frame %llu)\n", path, (unsigned long long)frame);
    { const char *d = getenv("RB_MEMDUMP_LOAD"); if (d) mem_dump_all(d); }   /* test: guest memory right after loading */
}

/* Load by restarting this program with the state: a clean process, nothing of the current run left over. */
static char **saved_argv;
void state_set_argv(char **argv) { saved_argv = argv; }
void state_load_restart(const char *path) {
    char full[PATH_MAX];
    if (!realpath(path, full)) { fprintf(stderr, "state: %s not found\n", path); return; }
    if (access(full, R_OK)) { fprintf(stderr, "state: cannot read %s\n", full); return; }
    setenv("RB_LOAD_STATE", full, 1);
    fprintf(stderr, "state: restarting to load %s\n", full);
    fflush(stderr);
    execv("/proc/self/exe", saved_argv);
    fatal("cannot restart to load the state (%s)", strerror(errno));
}

/* The quick slot (window hotkeys, menu): <data folder>/states/quick.state */
void state_quick_path(char *out, size_t n) {
    char d[600]; snprintf(d, sizeof d, "%s/states", data_folder()); mkdir(d, 0755);
    snprintf(out, n, "%s/quick.state", d);
}
