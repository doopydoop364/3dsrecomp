/* Recompiled CRO modules. A module's code is compiled once, position-independent (generated code adds the module's
 * current base, *mb, to every address it computes), so it runs wherever the game loads it. ldr_ro.c reports loads
 * and unloads; recomp_lookup() asks here for addresses outside the main program.
 * A module loaded without compiled code is saved (pristine) to <work>/cro/<name>.cro and listed in
 * missing_modules.txt: the next build (3dsrecomp run does it) compiles it. */
#include "host.h"
#include <sys/stat.h>


/* no modules unless the game's generated code provides its table (gen/modules.c) */
__attribute__((weak)) const RecompModule recomp_modules[1];
__attribute__((weak)) const uint32_t recomp_module_count = 0;

typedef struct { uint32_t base, size; const RecompModule *rm; recomp_fn *arm, *thumb; } Loaded;
static Loaded loaded[64]; static int nloaded;
extern const char *host_game_dir;

static void module_name_at(uint32_t m, char *out, size_t n) {
    uint32_t a = ld32(m + 0x80 + 4 * 16), k = 0;            /* module name offset field (a file offset before rebase) */
    if (a < m) a += m;
    for (; k + 1 < n; k++) { out[k] = (char)ld8(a + k); if (!out[k]) break; }
    out[k] = 0;
}
static void work_dir(char *out, size_t n) {                /* <work>/<id>: the parent of the game folder */
    snprintf(out, n, "%s", host_game_dir);
    char *s = strrchr(out, '/'); if (s) *s = 0; else snprintf(out, n, ".");
}
void modules_capture(uint32_t base, uint32_t size) {
    char name[128], dir[400], path[600];
    module_name_at(base, name, sizeof name);
    for (uint32_t i = 0; i < recomp_module_count; i++) if (!strcmp(recomp_modules[i].name, name)) return;
    work_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/cro", dir); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/cro/%s.cro", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite((void *)(uintptr_t)base, 1, size, f); fclose(f); }
    LOG(1, "modules: \"%s\" has no recompiled code yet; saved to %s", name, path);
}
void modules_loaded(uint32_t base, uint32_t size) {
    char name[128];
    module_name_at(base, name, sizeof name);
    const RecompModule *rm = 0;
    for (uint32_t i = 0; i < recomp_module_count; i++) if (!strcmp(recomp_modules[i].name, name)) rm = &recomp_modules[i];
    if (!rm) {
        char path[600]; snprintf(path, sizeof path, "%s/missing_modules.txt", host_game_dir);
        FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", name); fclose(f); }
        fatal("module \"%s\" has no recompiled code yet; it was saved for the next build (3dsrecomp run rebuilds and restarts)", name);
    }
    if (nloaded == 64) fatal("too many modules loaded");
    Loaded *l = &loaded[nloaded++];
    l->base = base; l->size = size; l->rm = rm;
    l->arm = calloc(size / 4 + 1, sizeof *l->arm); l->thumb = calloc(size / 2 + 1, sizeof *l->thumb);
    for (uint32_t i = 0; i < rm->nfns; i++) {
        uint32_t o = rm->fns[i].off;
        if ((o & ~1u) >= size) continue;
        if (o & 1) l->thumb[(o & ~1u) >> 1] = rm->fns[i].fn; else l->arm[o >> 2] = rm->fns[i].fn;
    }
    *rm->mb = base;
    LOG(1, "modules: \"%s\" runs recompiled at %08x (%u functions)", name, base, rm->nfns);
}
void modules_unloaded(uint32_t base) {
    for (int i = 0; i < nloaded; i++)
        if (loaded[i].base == base) { free(loaded[i].arm); free(loaded[i].thumb); loaded[i] = loaded[--nloaded]; return; }
}
/* Code of a loaded module that its analysis missed: <work>/cro/<name>.extra gets its offset (a seed for the next
 * build) and missing_modules.txt the module, so 3dsrecomp run rebuilds and restarts. Returns 0 if not in a module. */
int modules_note_missing(uint32_t a) {
    for (int i = 0; i < nloaded; i++) {
        Loaded *l = &loaded[i];
        uint32_t o = (a & ~1u) - l->base;
        if (o >= l->size) continue;
        char dir[400], path[600]; work_dir(dir, sizeof dir);
        snprintf(path, sizeof path, "%s/cro", dir); mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cro/%s.extra", dir, l->rm->name);
        FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%08x\n", o | (a & 1)); fclose(f); }
        snprintf(path, sizeof path, "%s/missing_modules.txt", host_game_dir);
        f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", l->rm->name); fclose(f); }
        LOG(1, "modules: \"%s\" offset %x was not found by the analysis; noted for the next build", l->rm->name, o | (a & 1));
        return 1;
    }
    return 0;
}
recomp_fn recomp_resume_module(uint32_t key) {
    for (int i = 0; i < nloaded; i++) {
        Loaded *l = &loaded[i];
        uint32_t o = (key & ~1u) - l->base;
        if (o >= l->size) continue;
        const ModFn *r = l->rm->res; uint32_t lo = 0, hi = *l->rm->nres, k = o | (key & 1);
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (r[m].off < k) lo = m + 1; else hi = m; }
        *l->rm->mb = l->base;
        return lo < *l->rm->nres && r[lo].off == k ? r[lo].fn : 0;
    }
    return 0;
}
recomp_fn recomp_lookup_module(uint32_t a) {
    for (int i = 0; i < nloaded; i++) {
        Loaded *l = &loaded[i];
        uint32_t o = (a & ~1u) - l->base;
        if (o >= l->size) continue;
        *l->rm->mb = l->base;                                   /* a module loaded twice in a row keeps the right base */
        return (a & 1) ? l->thumb[o >> 1] : (a & 3) ? 0 : l->arm[o >> 2];
    }
    return 0;
}
