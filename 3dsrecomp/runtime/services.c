/* System services reached through IPC (svcSendSyncRequest). Each handler rewrites the command
 * buffer in place with the reply. Commands that are not implemented yet answer "success" and are
 * logged as STUB so they are easy to find. */
#define _GNU_SOURCE
#include "host.h"
#include "input.h"
#include "pica.h"
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *game_dir;
/* System data shared by all games (shared font, system archives): <game>/sysarchives/<name>, else the folder in
 * $RB_SYSTEM_DIR (3dsrecomp run sets it), else system/sysarchives/<name> under the current folder. */
static void sys_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/sysarchives/%s", game_dir, name);
    if (!access(out, R_OK)) return;
    const char *sd = getenv("RB_SYSTEM_DIR");
    snprintf(out, n, "%s/sysarchives/%s", sd ? sd : "system", name);
}

static void reply(uint32_t *cmd, int normal, int translate) {
    cmd[0] = (cmd[0] & 0xffff0000u) | ((uint32_t)normal << 6) | (uint32_t)translate;
    cmd[1] = RES_OK;
}
static void stub(KObject *s, uint32_t *cmd) {
    LOG(1, "STUB %-9s cmd %08x  args %08x %08x %08x %08x", s->name, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4]);
    reply(cmd, 1, 0);
    memset(cmd + 2, 0, 6 * 4);                 /* outputs a caller may read anyway: zero, not its own arguments */
}

/* ---- srv: the service manager ---- */
static void srv(KObject *s, uint32_t *cmd) {
    switch (cmd[0] >> 16) {
    case 0x0001: reply(cmd, 1, 0); break;                              /* RegisterClient */
    case 0x0002: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(k_semaphore(0, 64)); break;   /* EnableNotification */
    case 0x0005: {                                                     /* GetServiceHandle */
        char name[9] = { 0 };
        memcpy(name, &cmd[1], cmd[3] < 8 ? cmd[3] : 8);
        KObject *o = k_session(name);
        LOG(1, "srv: GetServiceHandle \"%s\"%s", name, o->handler == stub ? "  (no implementation yet)" : "");
        reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(o);
        break;
    }
    case 0x0009: case 0x000a: case 0x000c: reply(cmd, 1, 0); break;    /* Subscribe, Unsubscribe, PublishToSubscriber */
    case 0x000b: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* ReceiveNotification */
    default: stub(s, cmd);
    }
}

/* The shared font lives right after the application's memory in the linear mapping (0x18000000 for an Old 3DS
 * title with its linear heap at 0x14000000). Its CFNT holds absolute pointers for the address it was dumped at;
 * move them to the new one (as Azahar's BCFNT::RelocateSharedFont). */
static void font_relocate(uint8_t *m, uint32_t size, uint32_t target) {
    #define R32(o) (*(uint32_t *)(m + (o)))
    uint32_t cfnt = 0x80;
    if (size < cfnt + 0x14 || (memcmp(m + cfnt, "CFNT", 4) && memcmp(m + cfnt, "CFNU", 4))) { LOG(1, "apt: shared font has no CFNT/CFNU, not relocated"); return; }
    uint32_t hsz = *(uint16_t *)(m + cfnt + 6), nblocks = R32(cfnt + 0x10);
    uint32_t first_cmap = 0, assumed_cmap = 0, o = cfnt + hsz;
    for (uint32_t b = 0; b < nblocks && o + 8 <= size; b++) {             /* find where the pointers assume the base is */
        if (!first_cmap && !memcmp(m + o, "CMAP", 4)) first_cmap = o;
        else if (!memcmp(m + o, "FINF", 4)) assumed_cmap = R32(o + 24) - 8;
        o += R32(o + 4);
    }
    uint32_t prev = assumed_cmap - first_cmap, delta = target - prev;
    if (!first_cmap || !delta) return;
    o = cfnt + hsz;
    for (uint32_t b = 0; b < nblocks && o + 8 <= size; b++) {
        if (!memcmp(m + o, "FINF", 4)) { R32(o + 16) += delta; R32(o + 20) += delta; R32(o + 24) += delta; }
        else if (!memcmp(m + o, "CMAP", 4)) { if (R32(o + 16)) R32(o + 16) += delta; }
        else if (!memcmp(m + o, "CWDH", 4)) { if (R32(o + 12)) R32(o + 12) += delta; }
        else if (!memcmp(m + o, "TGLP", 4)) R32(o + 28) += delta;
        o += R32(o + 4);
    }
    LOG(1, "apt: shared font moved from %08x to %08x", prev, target);
    #undef R32
}

/* ---- APT: applet manager. The game is the only running application and is never suspended. ---- */
static KObject *apt_lock, *apt_notify_ev, *apt_param_ev;
static int apt_param_pending;                 /* a Wakeup parameter is waiting to be received */
static uint32_t apt_cpu_percent = 30;

static void apt(KObject *s, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0001:                                                       /* GetLockHandle */
        if (!apt_lock) apt_lock = k_mutex();
        { uint32_t attr = cmd[1]; reply(cmd, 3, 2); cmd[2] = attr; cmd[3] = 0; cmd[4] = 0; cmd[5] = h_new(apt_lock); }
        break;
    case 0x0002:                                                       /* Initialize */
        if (!apt_notify_ev) { apt_notify_ev = k_event(0); apt_param_ev = k_event(0); }
        apt_param_pending = 1; k_signal(apt_param_ev);                 /* tell the application to wake up */
        reply(cmd, 1, 3); cmd[2] = 0x04000000; cmd[3] = h_new(apt_notify_ev); cmd[4] = h_new(apt_param_ev);
        break;
    case 0x0003: reply(cmd, 1, 0); break;                              /* Enable */
    case 0x0006: { uint32_t app = cmd[1]; reply(cmd, 7, 0); cmd[2] = RECOMP_TITLE_ID_LO; cmd[3] = RECOMP_TITLE_ID_HI; cmd[4] = 1; cmd[5] = app == 0x300; cmd[6] = app == 0x300; cmd[7] = 0; break; }
    case 0x0009: { uint32_t app = cmd[1]; reply(cmd, 2, 0); cmd[2] = app == 0x300; break; }   /* IsRegistered */
    case 0x000b: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* InquireNotification: none */
    case 0x000d: case 0x000e: {                                        /* ReceiveParameter / GlanceParameter */
        uint32_t *sb = staticbufs();
        if (!apt_param_pending) { reply(cmd, 1, 0); cmd[1] = 0xc8a15fefu; break; }
        if (id == 0x000d) apt_param_pending = 0;
        reply(cmd, 4, 4);
        cmd[2] = 0; cmd[3] = 1 /* Wakeup */; cmd[4] = 0; cmd[5] = 0x10; cmd[6] = 0; cmd[7] = 2; cmd[8] = sb[1];
        break;
    }
    case 0x0044: {                                                     /* GetSharedFont */
        static KObject *font;
        if (!font) {
            font = k_shmem(0x332000); font->prefer = LINEAR_BASE + app_mem_size;   /* after the application's memory */
            char fp[600]; sys_path(fp, sizeof fp, "shared_font.bin");
            FILE *f = fopen(fp, "rb");
            size_t n = f ? fread(font->mem, 1, font->size, f) : 0;
            if (f) fclose(f);
            if (n) { LOG(1, "apt: shared system font loaded from %s (%zu bytes)", fp, n); font_relocate(font->mem, (uint32_t)n, font->prefer); }
            else fatal("the game needs the shared system font, and %s is missing.\n"
                       "Build it with extract/make_shared_font.py from the font system archive dumped from your console\n"
                       "(title 0004009b00014002 on USA/EUR/JPN consoles).", fp);
        }
        reply(cmd, 2, 2); cmd[2] = font->prefer; cmd[3] = 0; cmd[4] = h_new(font);
        break;
    }
    case 0x0043: case 0x0055: reply(cmd, 1, 0); break;                 /* NotifyToWait, SetApplicationCpuTimeLimit */
    case 0x004b: reply(cmd, 2, 2); cmd[2] = 0; cmd[3] = 2; cmd[4] = staticbufs()[1]; break;   /* AppletUtility */
    case 0x004f: apt_cpu_percent = cmd[2]; reply(cmd, 1, 0); break;    /* SetAppCpuTimeLimit */
    case 0x0050: reply(cmd, 2, 0); cmd[2] = apt_cpu_percent; break;    /* GetAppCpuTimeLimit */
    case 0x0051: reply(cmd, 2, 2); cmd[2] = 0; cmd[3] = 2; cmd[4] = staticbufs()[1]; break;   /* GetStartupArgument: none */
    case 0x0101: case 0x0102: reply(cmd, 2, 0); cmd[2] = (uint32_t)is_new_3ds; break;   /* CheckNew3DSApp / CheckNew3DS */
    default: stub(s, cmd);
    }
}

/* ---- cfg: system configuration ---- */
static void cfg(KObject *s, uint32_t *cmd) {
    switch (cmd[0] >> 16) {
    case 0x0001: {                                                     /* GetConfigInfoBlk2 */
        uint32_t size = cmd[1], blk = cmd[2], desc = cmd[3], ptr = cmd[4];
        uint8_t *out = (uint8_t *)(uintptr_t)ptr;
        memset(out, 0, size);
        switch (blk) {
        case 0x00050005: { static const float cam[8] = { 62.0f, 289.0f, 76.80000305f, 46.08000183f, 10.0f, 5.0f, 55.58000183f, 21.56999969f };
                           if (!getenv("RB_TEST_NOSTEREO")) memcpy(out, cam, size < sizeof cam ? size : sizeof cam);
                           LOG(1, "cfg: stereo camera block requested (%u bytes)%s", size, getenv("RB_TEST_NOSTEREO") ? ", answered with zeros (test)" : ""); break; }
        case 0x00070001: out[0] = 1; break;                            /* sound output: stereo */
        case 0x00090001: { uint64_t uid = 0x0123456789abcdefull; memcpy(out, &uid, size < 8 ? size : 8); break; }
        case 0x000a0000: { static const uint16_t name[] = { 'P', 'l', 'a', 'y', 'e', 'r' }; memcpy(out, name, size < sizeof name ? size : sizeof name); break; }
        case 0x000a0001: if (size >= 2) { out[0] = 1; out[1] = 1; } break;   /* birthday */
        case 0x000a0002: out[0] = 1; break;                            /* language: English */
        case 0x000b0000: if (size >= 4) { out[2] = 1; out[3] = 49; } break; /* country: USA */
        case 0x000d0000: if (size >= 4) { out[0] = 0xff; out[1] = 0xff; out[2] = 0xff; out[3] = 0xff; } break; /* EULA accepted */
        case 0x000f0004: break;                                        /* console model: original 3DS */
        default: LOG(1, "cfg: config block %08x (size %x) is not known, returning zeros", blk, size);
        }
        reply(cmd, 1, 2); cmd[2] = desc; cmd[3] = ptr;
        break;
    }
    case 0x0002: reply(cmd, 2, 0); cmd[2] = 1; break;                  /* SecureInfoGetRegion: USA */
    case 0x0003: reply(cmd, 3, 0); cmd[2] = 0x89abcdef ^ cmd[1]; cmd[3] = 0x01234567; break;   /* GenHashConsoleUnique */
    case 0x0004: reply(cmd, 2, 0); cmd[2] = 1; break;                  /* GetRegionCanadaUSA */
    case 0x0005: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetSystemModel: original 3DS */
    case 0x0006: reply(cmd, 2, 0); cmd[2] = 1; break;                  /* GetModelNintendo2DS: not a 2DS */
    default: stub(s, cmd);
    }
}

/* ---- ptm: power and pedometer ---- */
static void ptm(KObject *s, uint32_t *cmd) {
    switch (cmd[0] >> 16) {
    case 0x0005: reply(cmd, 2, 0); cmd[2] = 1; break;                  /* GetAdapterState: plugged in */
    case 0x0006: reply(cmd, 2, 0); cmd[2] = 1; break;                  /* GetShellState: open */
    case 0x0007: reply(cmd, 2, 0); cmd[2] = 5; break;                  /* GetBatteryLevel: full */
    case 0x0008: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetBatteryChargeState */
    case 0x0009: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetPedometerState */
    case 0x000c: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetTotalStepCount */
    case 0x040a: reply(cmd, 2, 0); cmd[2] = (uint32_t)is_new_3ds; break;   /* ptm:sysm CheckNew3DS */
    default: stub(s, cmd);
    }
}

static void quiet_ok(KObject *s, uint32_t *cmd) {
    LOG(2, "ok   %-9s cmd %08x  args %08x %08x %08x", s->name, cmd[0], cmd[1], cmd[2], cmd[3]);
    reply(cmd, 1, 0);
}

/* ---- fs:USER: RomFS comes from the user's dump, save data lives in plain host folders ---- */
#define FS_NOT_FOUND 0xc8804478u
#define FS_EXISTS    0xc82044beu
typedef struct { int fd; uint64_t base, size; int fixed, virt; } HFile;   /* virt: the game RomFS (romfs.c) */
uint64_t romfs_open(const char *dir); int64_t romfs_read(uint64_t off, uint32_t size, uint8_t *dst);
typedef struct { uint32_t id; char root[300]; } HArchive;
static HArchive archives[32];
static int narchives;

/* Guest path (ASCII or UTF-16) -> host path under root. Returns 0 for paths that try to escape. */
static int fs_path(const char *root, uint32_t type, uint32_t size, uint32_t ptr, char *out, size_t outsz) {
    char rel[256]; size_t n = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)ptr;
    if (type == 3) for (uint32_t i = 0; i < size && p[i] && n < sizeof rel - 1; i++) rel[n++] = (char)p[i];
    else if (type == 4) for (uint32_t i = 0; i + 1 < size && (p[i] | p[i + 1]) && n < sizeof rel - 1; i += 2) rel[n++] = p[i + 1] ? '_' : (char)p[i];
    rel[n] = 0;
    if (strstr(rel, "..")) return 0;
    snprintf(out, outsz, "%s%s%s", root, rel[0] == '/' ? "" : "/", rel);
    return 1;
}
static void file_session(KObject *s, uint32_t *cmd) {
    HFile *f = s->user;
    switch (cmd[0] >> 16) {
    case 0x0802: {                                                     /* Read */
        uint64_t off = cmd[1] | ((uint64_t)cmd[2] << 32); uint32_t size = cmd[3], desc = cmd[4], ptr = cmd[5];
        if (off > f->size) off = f->size;
        if (f->fixed && size > f->size - off) size = (uint32_t)(f->size - off);
        ssize_t n = f->virt ? (ssize_t)romfs_read(off, size, (uint8_t *)(uintptr_t)ptr) : pread(f->fd, (void *)(uintptr_t)ptr, size, (off_t)(f->base + off));
        LOG(3, "file read off %llx size %x -> %zd", (unsigned long long)off, size, n);
        { static int trace = -1; if (trace < 0) trace = getenv("RB_LOG_READS") != 0;
          if (trace) fprintf(stderr, "read frame %llu off %llx size %x -> %zd\n", (unsigned long long)gpu_frame(), (unsigned long long)off, size, n); }
        reply(cmd, 2, 2); cmd[2] = n < 0 ? 0 : (uint32_t)n; cmd[3] = desc; cmd[4] = ptr;
        break;
    }
    case 0x0803: {                                                     /* Write */
        uint64_t off = cmd[1] | ((uint64_t)cmd[2] << 32); uint32_t size = cmd[3], desc = cmd[5], ptr = cmd[6];
        ssize_t n = f->fixed ? -1 : pwrite(f->fd, (void *)(uintptr_t)ptr, size, (off_t)off);
        reply(cmd, 2, 2); cmd[2] = n < 0 ? 0 : (uint32_t)n; cmd[3] = desc; cmd[4] = ptr;
        break;
    }
    case 0x0804: {                                                     /* GetSize */
        uint64_t size = f->size; struct stat st;
        if (!f->fixed && !fstat(f->fd, &st)) size = (uint64_t)st.st_size;
        reply(cmd, 3, 0); cmd[2] = (uint32_t)size; cmd[3] = (uint32_t)(size >> 32);
        break;
    }
    case 0x0805: if (!f->fixed && ftruncate(f->fd, (off_t)(cmd[1] | ((uint64_t)cmd[2] << 32)))) LOG(1, "file SetSize failed"); reply(cmd, 1, 0); break;
    case 0x0808: if (!f->fixed) close(f->fd); reply(cmd, 1, 0); break; /* Close */
    case 0x0809: case 0x080a: reply(cmd, 1, 0); break;                 /* Flush, SetPriority */
    case 0x080b: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetPriority */
    case 0x080c: { KObject *o = k_session("file"); o->handler = file_session; o->user = f; reply(cmd, 1, 2); cmd[2] = 0x10; cmd[3] = h_new(o); break; }
    default: stub(s, cmd);
    }
}
static uint32_t file_open(HFile *f) {
    KObject *o = k_session("file"); o->handler = file_session; o->user = f;
    return h_new(o);
}
/* Directory handle: Read returns 0x228-byte entries (UTF-16 name, 8.3 name and extension, attributes, size). */
typedef struct { DIR *d; char path[600]; } HDir;
static void dir_session(KObject *s, uint32_t *cmd) {
    HDir *hd = s->user;
    switch (cmd[0] >> 16) {
    case 0x0801: {                                                     /* Read(count) -> entries */
        uint32_t count = cmd[1], desc = cmd[2], ptr = cmd[3], n = 0;
        struct dirent *e;
        while (hd && hd->d && n < count && (e = readdir(hd->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            uint8_t ent[0x228] = { 0 };
            char full[900]; struct stat st = { 0 };
            snprintf(full, sizeof full, "%s/%s", hd->path, e->d_name);
            stat(full, &st);
            int isdir = S_ISDIR(st.st_mode);
            size_t len = strlen(e->d_name);
            for (size_t i = 0; i < len && i < 261; i++) ent[i * 2] = (uint8_t)e->d_name[i];
            const char *dot = isdir ? 0 : strrchr(e->d_name, '.');
            size_t base = dot ? (size_t)(dot - e->d_name) : len;
            for (int i = 0; i < 8; i++) ent[0x20c + i] = i < (int)base ? (uint8_t)toupper((unsigned char)e->d_name[i]) : ' ';
            if (base > 8) { ent[0x20c + 6] = '~'; ent[0x20c + 7] = '1'; }
            for (int i = 0; i < 3; i++) ent[0x216 + i] = dot && dot[1 + i] ? (uint8_t)toupper((unsigned char)dot[1 + i]) : ' ';
            ent[0x21c] = (uint8_t)isdir; ent[0x21e] = (uint8_t)!isdir;
            uint64_t size = isdir ? 0 : (uint64_t)st.st_size; memcpy(ent + 0x220, &size, 8);
            memcpy((uint8_t *)(uintptr_t)ptr + n * 0x228, ent, sizeof ent);
            n++;
        }
        reply(cmd, 2, 2); cmd[2] = n; cmd[3] = desc; cmd[4] = ptr; break;
    }
    case 0x0802: if (hd) { if (hd->d) closedir(hd->d); free(hd); s->user = 0; } reply(cmd, 1, 0); break;   /* Close */
    default: stub(s, cmd);
    }
}
static int rm_entry(const char *p, const struct stat *st, int flag, struct FTW *ftw) { (void)st; (void)flag; (void)ftw; return remove(p); }
static int rm_tree(const char *p) { return nftw(p, rm_entry, 16, FTW_DEPTH | FTW_PHYS); }
static HFile romfs;
#define FS_NOT_FORMATTED 0xc8a04554u
#define FS_ARCHIVE_NOT_FOUND 0xc8a04478u
static void extdata_root(char *out, size_t n, int shared, uint32_t lo, uint32_t hi) {
    snprintf(out, n, "%s/%s", game_dir, shared ? "extdata_shared" : "extdata"); mkdir(out, 0755);
    snprintf(out, n, "%s/%s/%08x%08x", game_dir, shared ? "extdata_shared" : "extdata", hi, lo);
}
/* Opens an archive. Save data always exists; extra data must have been created first, as on a console. */
static uint32_t archive_open(uint32_t id, uint32_t psize, uint32_t pptr, HArchive **out) {
    if (narchives == 32) fatal("too many open archives");
    HArchive *a = &archives[narchives];
    a->id = id; a->root[0] = 0;
    struct stat st;
    if (id == 6 || id == 7 || id == 0x12345678) {                      /* ExtSaveData, SharedExtSaveData, BOSS */
        uint32_t lo = psize >= 8 ? ld32(pptr + 4) : 0, hi = psize >= 12 ? ld32(pptr + 8) : 0;
        extdata_root(a->root, sizeof a->root, id == 7, lo, hi);
        if (stat(a->root, &st)) {
            if (id == 7) {                                             /* shared extra data is created by the system on every console */
                mkdir(a->root, 0755);
                LOG(1, "fs: created empty shared extra data %08x%08x (the system provides it on a real console)", hi, lo);
            } else { LOG(1, "fs: extra data %08x%08x does not exist", hi, lo); return FS_ARCHIVE_NOT_FOUND; }
        }
    } else if ((id == 0x567890b2 || id == 0x567890b4) && psize >= 8 &&
               (ld32(pptr + 4) & 0xffffff) != (uint32_t)((RECOMP_TITLE_ID >> 8) & 0xffffff)) {
        /* another title's save data (path: media type, unique id): that game's save if it is also in work/ */
        uint32_t lo = (ld32(pptr + 4) & 0xffffff) << 8, hi = 0x00040000u;
        char gd[300]; snprintf(gd, sizeof gd, "%s", game_dir);
        char *s1 = strrchr(gd, '/'); if (s1) *s1 = 0;                 /* work/<id>/game -> work/<id> */
        char *s2 = strrchr(gd, '/'); if (s2) *s2 = 0;                 /* -> work */
        snprintf(a->root, sizeof a->root, "%s/%08x%08x/game/save", s1 && s2 ? gd : ".", hi, lo);
        if (stat(a->root, &st)) { LOG(1, "fs: save data of title %08x%08x does not exist", hi, lo); return FS_ARCHIVE_NOT_FOUND; }
    } else if (id == 4 || id == 0x567890b2 || id == 0x567890b4) {      /* the game's save data: exists once formatted */
        snprintf(a->root, sizeof a->root, "%s/save", game_dir);
        if (stat(a->root, &st)) { LOG(1, "fs: save data is not formatted yet"); return FS_NOT_FORMATTED; }
    } else if (id == 8) {
        snprintf(a->root, sizeof a->root, "%s/save", game_dir); mkdir(a->root, 0755);
    } else if (id == 9) {
        snprintf(a->root, sizeof a->root, "%s/sdmc", game_dir); mkdir(a->root, 0755);
    }
    narchives++; *out = a;
    return RES_OK;
}
static HArchive *archive_get(uint32_t lo) { return lo >= 1 && lo <= (uint32_t)narchives ? &archives[lo - 1] : 0; }
static uint32_t host_open(HArchive *a, uint32_t ptype, uint32_t psize, uint32_t pptr, uint32_t flags, uint32_t *out_handle) {
    char path[600];
    if (!a || !a->root[0] || !fs_path(a->root, ptype, psize, pptr, path, sizeof path)) return FS_NOT_FOUND;
    int fd = open(path, ((flags & 2) ? O_RDWR : O_RDONLY) | ((flags & 4) ? O_CREAT : 0), 0644);
    LOG(1, "fs: open %s flags %x -> %s", path, flags, fd < 0 ? "not found" : "ok");
    if (fd < 0) return FS_NOT_FOUND;
    HFile *f = calloc(1, sizeof *f); f->fd = fd;
    *out_handle = file_open(f);
    return RES_OK;
}
static void fs_user(KObject *s, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16, res, h = 0;
    char path[600];
    switch (id) {
    case 0x0801: case 0x0861: case 0x0862: case 0x080e: case 0x080d: reply(cmd, 1, 0); break;
    case 0x0802:                                                       /* OpenFile */
        res = host_open(archive_get(cmd[2]), cmd[4], cmd[5], cmd[9], cmd[6], &h);
        reply(cmd, 1, 2); cmd[1] = res; cmd[2] = 0x10; cmd[3] = h; break;
    case 0x0803: {                                                     /* OpenFileDirectly */
        uint32_t arch = cmd[2], ftype = cmd[5], fsize = cmd[6], flags = cmd[7], fptr = cmd[12];
        if (arch == 3) {                                               /* this title's own data: RomFS */
            uint32_t kind = fsize >= 4 ? ld32(fptr) : 0;
            if (kind != 0) LOG(1, "fs: SelfNCCH section %u requested, serving RomFS", kind);
            LOG(1, "fs: open RomFS (%llu MB)", (unsigned long long)(romfs.size >> 20));
            res = RES_OK; h = file_open(&romfs);
        } else {
            HArchive *a = 0;
            if (arch == 0x2345678a) {                                  /* content of another (system) title */
                uint32_t ap = cmd[10];
                char nm[64], sp[600]; snprintf(nm, sizeof nm, "%08x%08x.romfs", ld32(ap + 4), ld32(ap)); sys_path(sp, sizeof sp, nm);
                int fd = open(sp, O_RDONLY); struct stat st;
                if (fd >= 0 && !fstat(fd, &st)) {                      /* user-supplied dump of the system archive's RomFS */
                    HFile *f = calloc(1, sizeof *f); f->fd = fd; f->size = (uint64_t)st.st_size; f->fixed = 1;
                    char magic[4] = { 0 };
                    if (pread(fd, magic, 4, 0) == 4 && !memcmp(magic, "IVFC", 4)) { f->base = 0x1000; f->size -= 0x1000; }   /* skip the hash header */
                    LOG(1, "fs: system archive %s", sp);
                    reply(cmd, 1, 2); cmd[2] = 0x10; cmd[3] = file_open(f); break;
                }
                LOG(1, "fs: the game asks for system title %08x%08x, expected at %s", ld32(ap + 4), ld32(ap), sp);
                if (getenv("RB_FAKE_SYSARCHIVES")) {                   /* debugging aid only: an all-zero file */
                    HFile *f = calloc(1, sizeof *f); f->fd = open("/dev/zero", O_RDONLY); f->size = 0x100000; f->fixed = 1;
                    LOG(1, "fs: RB_FAKE_SYSARCHIVES is set, serving zeros instead");
                    reply(cmd, 1, 2); cmd[2] = 0x10; cmd[3] = file_open(f); break;
                }
            }
            res = archive_open(arch, cmd[4], cmd[10], &a);
            if (res == RES_OK) res = host_open(a, ftype, fsize, fptr, flags, &h);
        }
        reply(cmd, 1, 2); cmd[1] = res; cmd[2] = 0x10; cmd[3] = h; break;
    }
    case 0x0804:                                                       /* DeleteFile */
        res = archive_get(cmd[2]) && fs_path(archive_get(cmd[2])->root, cmd[4], cmd[5], cmd[7], path, sizeof path) && !unlink(path) ? RES_OK : FS_NOT_FOUND;
        reply(cmd, 1, 0); cmd[1] = res; break;
    case 0x0808: {                                                     /* CreateFile */
        HArchive *a = archive_get(cmd[2]); res = FS_NOT_FOUND;
        if (a && a->root[0] && fs_path(a->root, cmd[4], cmd[5], cmd[10], path, sizeof path)) {
            int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
            if (fd >= 0) { if (ftruncate(fd, (off_t)(cmd[7] | ((uint64_t)cmd[8] << 32)))) LOG(1, "fs: cannot size %s", path); close(fd); res = RES_OK; }
            else res = errno == EEXIST ? FS_EXISTS : FS_NOT_FOUND;
            LOG(1, "fs: create %s -> %08x", path, res);
        }
        reply(cmd, 1, 0); cmd[1] = res; break;
    }
    case 0x0809: {                                                     /* CreateDirectory */
        HArchive *a = archive_get(cmd[2]); res = FS_NOT_FOUND;
        if (a && a->root[0] && fs_path(a->root, cmd[4], cmd[5], cmd[8], path, sizeof path)) res = !mkdir(path, 0755) ? RES_OK : errno == EEXIST ? FS_EXISTS : FS_NOT_FOUND;
        reply(cmd, 1, 0); cmd[1] = res; break;
    }
    case 0x0805: case 0x080a: {                                        /* RenameFile, RenameDirectory */
        HArchive *a = archive_get(cmd[2]), *b = archive_get(cmd[6]); char to[600]; res = FS_NOT_FOUND;
        if (a && b && a->root[0] && b->root[0] && fs_path(a->root, cmd[4], cmd[5], cmd[11], path, sizeof path) &&
            fs_path(b->root, cmd[8], cmd[9], cmd[13], to, sizeof to)) {
            struct stat st;
            if (!stat(to, &st)) res = FS_EXISTS;
            else if (!rename(path, to)) res = RES_OK;
        }
        LOG(1, "fs: rename %s -> %08x", path, res);
        reply(cmd, 1, 0); cmd[1] = res; break;
    }
    case 0x0806: case 0x0807: {                                        /* DeleteDirectory, DeleteDirectoryRecursively */
        HArchive *a = archive_get(cmd[2]); res = FS_NOT_FOUND;
        if (a && a->root[0] && fs_path(a->root, cmd[4], cmd[5], cmd[7], path, sizeof path) && strcmp(path, a->root) &&
            !(id == 0x0806 ? rmdir(path) : rm_tree(path))) res = RES_OK;
        LOG(1, "fs: delete directory %s -> %08x", path, res);
        reply(cmd, 1, 0); cmd[1] = res; break;
    }
    case 0x080b: {                                                     /* OpenDirectory */
        HArchive *a = archive_get(cmd[1]); HDir *hd = calloc(1, sizeof *hd);
        if (a && a->root[0] && fs_path(a->root, cmd[3], cmd[4], cmd[6], hd->path, sizeof hd->path)) hd->d = opendir(hd->path);
        if (!hd->d) { free(hd); reply(cmd, 1, 0); cmd[1] = FS_NOT_FOUND; break; }
        KObject *o = k_session("dir"); o->handler = dir_session; o->user = hd;
        reply(cmd, 1, 2); cmd[2] = 0x10; cmd[3] = h_new(o); break;
    }
    case 0x080c: {                                                     /* OpenArchive */
        HArchive *a = 0; uint32_t aid = cmd[1];
        if (aid >> 8 == 0x567890 && cmd[3] >= 12) LOG(2, "fs: OpenArchive %x path %08x %08x %08x", aid, ld32(cmd[5]), ld32(cmd[5] + 4), ld32(cmd[5] + 8));
        res = archive_open(aid, cmd[3], cmd[5], &a);
        LOG(1, "fs: OpenArchive id %x -> %s", aid, res ? "does not exist" : a->root[0] ? a->root : "(no host folder)");
        reply(cmd, 3, 0); cmd[1] = res; cmd[2] = res ? 0 : (uint32_t)narchives; cmd[3] = 0; break;
    }
    case 0x0851: { char root[300]; extdata_root(root, sizeof root, 0, cmd[2], cmd[3]); mkdir(root, 0755);   /* CreateExtSaveData */
                   LOG(1, "fs: created extra data folder %s", root); reply(cmd, 1, 0); break; }
    case 0x0852: {                                                     /* DeleteExtSaveData(media, low, high) */
        char root[300]; struct stat st; extdata_root(root, sizeof root, cmd[1] == 0, cmd[2], cmd[3]);
        res = stat(root, &st) ? FS_ARCHIVE_NOT_FOUND : rm_tree(root) ? FS_NOT_FOUND : RES_OK;
        LOG(1, "fs: delete extra data %s -> %08x", root, res); reply(cmd, 1, 0); cmd[1] = res; break;
    }
    case 0x084c: case 0x080f: {                                        /* FormatSaveData, FormatThisUserSaveData: empty save folder */
        char root[300]; snprintf(root, sizeof root, "%s/save", game_dir);
        if (id == 0x080f || cmd[1] == 4 || cmd[1] == 0x567890b2) { rm_tree(root); mkdir(root, 0755); LOG(1, "fs: formatted save data %s", root); }
        reply(cmd, 1, 0); break;
    }
    case 0x0817: case 0x0818: reply(cmd, 2, 0); cmd[2] = 1; break;     /* IsSdmcDetected / IsSdmcWritable */
    case 0x0845: { char root[300]; struct stat st; snprintf(root, sizeof root, "%s/save", game_dir);   /* GetFormatInfo */
                   if (cmd[1] == 4 && stat(root, &st)) { reply(cmd, 1, 0); cmd[1] = FS_NOT_FORMATTED; break; } }
                 reply(cmd, 5, 0); cmd[2] = 0x20000; cmd[3] = 8; cmd[4] = 8; cmd[5] = 0; break;   /* GetFormatInfo */
    case 0x0863: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetPriority */
    default: stub(s, cmd);
    }
}

/* ---- hid:USER: input state is published through shared memory (layout per the Azahar sources) ----
 * pad 0x000 (8 entries of 16 bytes at 0x28), touch 0x0a8 (8 x 8 at 0xc8), accelerometer 0x108 (8 x 6 at
 * 0x128), gyroscope 0x158 (32 x 6 at 0x178). Each section: tick of last index wrap, previous tick, index of
 * the newest entry. Accelerometer: 512 = 1 g. RB_ACCEL="x,y,z" sets the resting reading. */
static KObject *hid_mem, *hid_ev[5];
static uint32_t pad_idx, acc_idx, gyro_idx, pad_state;
static int16_t accel_rest[3] = { 0, -512, 0 };
static uint64_t hid_ticks(void) { return (uint64_t)((unsigned __int128)now_ns() * 268111856u / 1000000000u); }
static void hid_section(uint8_t *b, uint32_t *idx, uint32_t count, uint32_t entry_off, uint32_t entry_size, const void *entry) {
    uint32_t i = *idx = (*idx + 1) % count;
    memcpy(b + 0x10, &i, 4);
    memcpy(b + entry_off + i * entry_size, entry, entry_size);
    if (i == 0) { uint64_t t = hid_ticks(); memcpy(b + 8, b, 8); memcpy(b, &t, 8); }
}
static void hid_pad_tick(void) {
    uint8_t *m = hid_mem->mem;
    InputState in; input_get(&in);
    uint32_t state = in.buttons & 0x0fffffffu & ~(uint32_t)(BTN_ZL | BTN_ZR);
    int r2 = in.cx * in.cx + in.cy * in.cy;
    if (r2 > 40 * 40) {                                                /* circle pad direction bits, 30/60 degree sectors */
        int ax = in.cx < 0 ? -in.cx : in.cx, ay = in.cy < 0 ? -in.cy : in.cy;
        if (ax * 1000 > ay * 577) state |= in.cx > 0 ? CIRCLE_RIGHT : CIRCLE_LEFT;     /* tan(30) */
        if (ay * 1000 > ax * 577) state |= in.cy > 0 ? CIRCLE_UP : CIRCLE_DOWN;
    }
    uint32_t pad[4] = { state, state & ~pad_state, pad_state & ~state, (uint32_t)(uint16_t)in.cx | (uint32_t)(uint16_t)in.cy << 16 };
    pad_state = state;
    memcpy(m + 0x1c, &pad_state, 4);
    hid_section(m, &pad_idx, 8, 0x28, 16, pad);
    uint32_t touch[2] = { in.touch ? (uint32_t)in.tx | (uint32_t)in.ty << 16 : 0, in.touch ? 1u : 0 };
    uint32_t tidx = pad_idx;
    memcpy(m + 0xa8 + 0x10, &tidx, 4);
    memcpy(m + 0xc8 + tidx * 8, touch, 8);
    if (tidx == 0) { uint64_t t = hid_ticks(); memcpy(m + 0xa8 + 8, m + 0xa8, 8); memcpy(m + 0xa8, &t, 8); }
    k_signal(hid_ev[0]); k_signal(hid_ev[1]);
}
static int16_t s16c(float v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static void hid_accel_tick(void) {                                     /* gamepad motion if any: 512 per g (Azahar) */
    uint8_t *m = hid_mem->mem;
    float a[3], g[3]; int16_t e[3] = { accel_rest[0], accel_rest[1], accel_rest[2] };
    if (input_get_motion(a, g)) for (int i = 0; i < 3; i++) e[i] = s16c(a[i] * 512.0f);
    int16_t raw[3] = { (int16_t)(-2 * e[0]), (int16_t)(-2 * e[2]), (int16_t)(2 * e[1]) };
    memcpy(m + 0x120, raw, 6);
    hid_section(m + 0x108, &acc_idx, 8, 0x20, 6, e);
    k_signal(hid_ev[2]);
}
static void hid_gyro_tick(void) {                                      /* 14.375 per degree/s (Azahar) */
    uint8_t *m = hid_mem->mem;
    float a[3], g[3]; int16_t e[3] = { 0, 0, 0 };
    if (input_get_motion(a, g)) for (int i = 0; i < 3; i++) e[i] = s16c(g[i] * 14.375f);
    memcpy(m + 0x170, e, 6);
    hid_section(m + 0x158, &gyro_idx, 32, 0x20, 6, e);
    k_signal(hid_ev[3]);
}
static void hid(KObject *s, uint32_t *cmd) {
    switch (cmd[0] >> 16) {
    case 0x000a:                                                       /* GetIPCHandles */
        if (!hid_mem) {
            hid_mem = k_shmem(0x1000);
            for (int i = 0; i < 5; i++) hid_ev[i] = k_event(0);
            const char *a = getenv("RB_ACCEL");
            if (a) sscanf(a, "%hd,%hd,%hd", &accel_rest[0], &accel_rest[1], &accel_rest[2]);
            k_add_periodic(hid_pad_tick, 1000000000ull / 234);
        }
        reply(cmd, 1, 7); cmd[2] = 0x14000000; cmd[3] = h_new(hid_mem);
        for (int i = 0; i < 5; i++) cmd[4 + i] = h_new(hid_ev[i]);
        break;
    case 0x0011: { static int on; if (!on++) k_add_periodic(hid_accel_tick, 1000000000ull / 104); LOG(1, "hid: accelerometer enabled"); reply(cmd, 1, 0); break; }
    case 0x0013: { static int on; if (!on++) k_add_periodic(hid_gyro_tick, 1000000000ull / 101); LOG(1, "hid: gyroscope enabled"); reply(cmd, 1, 0); break; }
    case 0x0012: case 0x0014: reply(cmd, 1, 0); break;               /* disable: keep publishing, harmless */
    case 0x0015: { float k = 14.375f; reply(cmd, 2, 0); memcpy(&cmd[2], &k, 4); break; }
    case 0x0016: { int16_t p[9] = { 0, 6700, -6700, 0, 6700, -6700, 0, 6700, -6700 };   /* gyroscope calibration */
                   reply(cmd, 6, 0); memset(&cmd[2], 0, 20); memcpy(&cmd[2], p, sizeof p); break; }
    case 0x0017: reply(cmd, 2, 0); cmd[2] = 0x3f; break;               /* GetSoundVolume: maximum */
    default: stub(s, cmd);
    }
}

void gsp_service(KObject *s, uint32_t *cmd);                      /* gpu.c */

/* ---- dsp::DSP: audio processor, high level (voices are decoded and mixed in dsp.c).
 * Implements the audio pipe handshake and the per-frame interrupt, so the game's sound library
 * starts up and keeps running; dsp.c decodes and mixes the voices and reports their status.
 * Protocol facts (pipe message, structure address list, frame period) follow the Azahar HLE. ---- */
#define DSP_MEM 0x1ff00000u
static KObject *dsp_sem_ev, *dsp_pipe_ev[4];
static uint8_t dsp_pipe[64];
static uint32_t dsp_pipe_len;
static int dsp_on, dsp_ticking;
static uint64_t dsp_frames;

void dsp_audio_frame(void);                                       /* dsp.c */
void y2r_service(KObject *s, uint32_t *cmd);                     /* y2r.c */
void nfc_service(KObject *s, uint32_t *cmd);                     /* nfc.c */
void nfc_set_dir(const char *game_dir);
static void dsp_tick(void) {                                           /* one 160-sample audio frame */
    if (!dsp_on) return;
    dsp_frames++;
    dsp_audio_frame();
    if (dsp_pipe_ev[2]) k_signal(dsp_pipe_ev[2]);
}
static void dsp_send_struct_addresses(void) {
    /* count, then the DSP addresses of the shared structures (0x8000 + byte offset / 2) */
    static const uint16_t list[16] = { 15, 0xbfff, 0x9e92, 0x8680, 0xa792, 0x9430, 0x8400, 0x8540, 0x9492,
                                       0x8710, 0x8410, 0xa912, 0xaa12, 0xaad2, 0xac52, 0xac5c };
    memcpy(dsp_pipe, list, sizeof list); dsp_pipe_len = sizeof list;
    if (dsp_pipe_ev[2]) k_signal(dsp_pipe_ev[2]);
}
static uint32_t dsp_pipe_read(uint32_t want, uint32_t ptr) {
    if (want > dsp_pipe_len) want = dsp_pipe_len;
    memcpy((void *)(uintptr_t)ptr, dsp_pipe, want);
    memmove(dsp_pipe, dsp_pipe + want, dsp_pipe_len - want); dsp_pipe_len -= want;
    return want;
}
static void dsp_cmd(KObject *s, uint32_t *cmd);
static void dsp(KObject *s, uint32_t *cmd) {
    uint32_t req[5]; memcpy(req, cmd, sizeof req);
    dsp_cmd(s, cmd);
    LOG(2, "dsp: %08x(%x, %x, %x, %x) -> %08x %x %x %x  [static buf %08x %08x]", req[0], req[1], req[2], req[3], req[4],
        cmd[1], cmd[2], cmd[3], cmd[4], staticbufs()[0], staticbufs()[1]);
}
static void dsp_cmd(KObject *s, uint32_t *cmd) {
    switch (cmd[0] >> 16) {
    case 0x0001: reply(cmd, 2, 0); cmd[2] = dsp_on ? 0 : 1; break;     /* RecvData(register 0): 0 while running */
    case 0x0002: reply(cmd, 2, 0); cmd[2] = 1; break;                  /* RecvDataIsReady */
    case 0x000c: { uint32_t a = cmd[1]; reply(cmd, 2, 0); cmd[2] = DSP_MEM + 0x40000u + (a << 1); break; }   /* ConvertProcessAddressFromDspDram */
    case 0x000d: {                                                     /* WriteProcessPipe */
        uint32_t ch = cmd[1], size = cmd[2]; const uint8_t *buf = (const uint8_t *)(uintptr_t)cmd[4];
        if (ch == 2 && size >= 4) {
            switch (buf[0]) {
            case 0: case 2:                                            /* Initialize / Wakeup */
                if (buf[0] == 0) memset((void *)(uintptr_t)DSP_MEM, 0, 0x80000);
                dsp_on = 1; dsp_send_struct_addresses();
                if (!dsp_ticking) { dsp_ticking = 1; k_add_periodic(dsp_tick, 4888700); }   /* 1310720 ARM11 cycles */
                LOG(1, "dsp: audio pipe %s, audio running", buf[0] ? "wakeup" : "initialize");
                break;
            case 1: dsp_on = 0; LOG(1, "dsp: audio pipe shutdown"); break;
            default: LOG(1, "dsp: audio pipe state %u is not implemented (sleep)", buf[0]);
            }
        } else LOG(1, "dsp: write of %u bytes to pipe %u ignored", size, ch);
        reply(cmd, 1, 0); break;
    }
    case 0x000e: { uint32_t want = cmd[3] & 0xffff, ptr = staticbufs()[1]; uint32_t n = cmd[1] == 2 ? dsp_pipe_read(want, ptr) : 0;
                   reply(cmd, 1, 2); cmd[2] = (n << 14) | 2; cmd[3] = ptr; break; }           /* ReadPipe */
    case 0x000f: { uint32_t ch = cmd[1]; reply(cmd, 2, 0); cmd[2] = ch == 2 ? dsp_pipe_len : 0; break; }   /* GetPipeReadableSize */
    case 0x0010: {                                                     /* ReadPipeIfPossible: all or nothing */
        uint32_t want = cmd[3] & 0xffff, ptr = staticbufs()[1];             /* size is a u16 */
        uint32_t n = (cmd[1] == 2 && want <= dsp_pipe_len) ? dsp_pipe_read(want, ptr) : 0;
        reply(cmd, 2, 2); cmd[2] = n; cmd[3] = (n << 14) | 2; cmd[4] = ptr; break;
    }
    case 0x0011: { uint32_t desc = cmd[4], ptr = cmd[5]; LOG(1, "dsp: LoadComponent (%u bytes of firmware, not executed)", cmd[1]);
                   reply(cmd, 2, 2); cmd[2] = 1; cmd[3] = desc; cmd[4] = ptr; break; }
    case 0x0015: {                                                     /* RegisterInterruptEvents(type, pipe, event) */
        uint32_t type = cmd[1], ch = cmd[2];
        if (type == 2 && ch < 4) dsp_pipe_ev[ch] = cmd[4] ? h_get(cmd[4]) : 0;
        else LOG(1, "dsp: interrupt type %u pipe %u registered but never raised", type, ch);
        reply(cmd, 1, 0); break;
    }
    case 0x0016: if (!dsp_sem_ev) dsp_sem_ev = k_event(0); reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(dsp_sem_ev); break;
    case 0x001f: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetHeadphoneStatus: not inserted */
    case 0x0007: case 0x0012: case 0x0013: case 0x0014: case 0x0017: case 0x0020:
        LOG(3, "dsp: cmd %08x %08x %08x", cmd[0], cmd[1], cmd[2]); reply(cmd, 1, 0); break;   /* semaphore, cache, unload */
    default: stub(s, cmd);
    }
}

/* ---- Network and social services of a console that is offline: no StreetPass, SpotPass, friends or Wi-Fi. ---- */
static KObject *offline_event(KObject **slot) { if (!*slot) *slot = k_event(0); return *slot; }
static void cecd(KObject *s, uint32_t *cmd) {                         /* cecd:u, StreetPass */
    static KObject *info_ev, *state_ev;
    switch (cmd[0] >> 16) {
    case 0x000e: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetCecdState: idle */
    case 0x000f: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(offline_event(&info_ev)); break;   /* GetCecInfoEventHandle */
    case 0x0010: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(offline_event(&state_ev)); break;  /* GetChangeStateEventHandle */
    case 0x0011: case 0x0012: {          /* OpenAndWrite / OpenAndRead(size, program id, path type, flags, pid, buffer): one file per box item */
        uint32_t size = cmd[1], desc = cmd[7], ptr = cmd[8], n = 0;
        char path[400]; snprintf(path, sizeof path, "%s/cecd", game_dir); mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/cecd/%08x_%u.bin", game_dir, cmd[2], cmd[3]);
        uint32_t res = RES_OK;
        if ((cmd[0] >> 16) == 0x0011) {
            FILE *f = fopen(path, "wb");
            if (f) { n = (uint32_t)fwrite((void *)(uintptr_t)ptr, 1, size, f); fclose(f); }
            LOG(1, "cecd: wrote %s (%u bytes)", path, n);
            reply(cmd, 1, 2); cmd[1] = res; cmd[2] = desc; cmd[3] = ptr;
        } else {
            FILE *f = fopen(path, "rb");
            if (f) { n = (uint32_t)fread((void *)(uintptr_t)ptr, 1, size, f); fclose(f); }
            else if (cmd[3] == 2 && size >= 0x60) {    /* box info: the title's StreetPass box exists and is empty */
                uint8_t *b = (uint8_t *)(uintptr_t)ptr; memset(b, 0, 0x60);
                b[0] = b[1] = 0x63; memcpy(b + 4, &cmd[2], 4); b[0x0c] = 1; n = 0x60;
            } else res = 0xc8810befu;                  /* NoData, CEC, NotFound (as Azahar) */
            LOG(1, "cecd: read %s -> %s", path, f ? "ok" : "not found");
            reply(cmd, 2, 2); cmd[1] = res; cmd[2] = n; cmd[3] = desc; cmd[4] = ptr;
        }
        break;
    }
    default: stub(s, cmd);
    }
}
static void boss(KObject *s, uint32_t *cmd) {                         /* boss:U, SpotPass: no tasks, nothing arrived */
    switch (cmd[0] >> 16) {
    case 0x0005: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetNewArrivalFlag */
    case 0x0007: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetOptoutFlag */
    case 0x000d: case 0x000e: case 0x0010: case 0x0011: case 0x0012:  /* task / step / ns data id lists: empty */
        reply(cmd, 2, 0); cmd[2] = 0; break;
    default: quiet_ok(s, cmd); memset(cmd + 2, 0, 6 * 4);
    }
}
static void frd(KObject *s, uint32_t *cmd) {                          /* frd:u, friends: not logged in, no friends */
    switch (cmd[0] >> 16) {
    case 0x0001: case 0x0002: reply(cmd, 2, 0); cmd[2] = 0; break;     /* HasLoggedIn, IsOnline */
    case 0x0011: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetFriendKeyList: none */
    case 0x0003: case 0x0004: case 0x0032: reply(cmd, 1, 0); break;    /* Login, Logout, SetClientSdkVersion */
    default:                                                           /* per-friend queries for an empty list: nothing to fill */
        LOG(2, "ok   %-9s cmd %08x", s->name, cmd[0]); reply(cmd, 1, 0); memset(cmd + 2, 0, 6 * 4);
    }
}
static void ac(KObject *s, uint32_t *cmd) {                           /* ac:u / ac:i, Wi-Fi: never connected */
    switch (cmd[0] >> 16) {
    case 0x0005: reply(cmd, 1, 0); cmd[1] = 0xe0a09d2eu; break;        /* GetConnectResult: failed */
    case 0x000c: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetLastErrorCode */
    case 0x000d: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* GetStatus: not connected */
    case 0x000e: case 0x0016: case 0x0017: reply(cmd, 2, 0); cmd[2] = 0; break;   /* GetWifiStatus, GetInfraPriority, ... */
    case 0x003e: reply(cmd, 2, 0); cmd[2] = 0; break;                  /* IsConnected: no */
    default: quiet_ok(s, cmd); memset(cmd + 2, 0, 6 * 4);
    }
}

/* ---- ir:rst: the New 3DS ZL / ZR buttons and C-stick (layout per Azahar) ---- */
static KObject *rst_mem, *rst_ev;
static uint32_t rst_idx = 7, rst_state; static int rst_on;
static void rst_tick(void) {
    if (!rst_on) return;
    uint8_t *m = rst_mem->mem;
    InputState in; input_get(&in);
    uint32_t st = in.buttons & (BTN_ZL | BTN_ZR);
    if (in.sx * in.sx + in.sy * in.sy > 40 * 40) {                   /* C-stick direction bits */
        int ax = in.sx < 0 ? -in.sx : in.sx, ay = in.sy < 0 ? -in.sy : in.sy;
        if (ax * 1000 > ay * 577) st |= in.sx > 0 ? 1u << 24 : 1u << 25;
        if (ay * 1000 > ax * 577) st |= in.sy > 0 ? 1u << 26 : 1u << 27;
    }
    uint32_t e[4] = { st, st & ~rst_state, rst_state & ~st, (uint32_t)(uint16_t)in.sx | (uint32_t)(uint16_t)in.sy << 16 };
    rst_state = st;
    hid_section(m, &rst_idx, 8, 0x18, 16, e);
    k_signal(rst_ev);
}
static void irrst(KObject *s, uint32_t *cmd) {
    if (!rst_mem) { rst_mem = k_shmem(0x1000); rst_ev = k_event(0); }
    switch (cmd[0] >> 16) {
    case 0x0001: reply(cmd, 1, 3); cmd[2] = 0x14000000; cmd[3] = h_new(rst_mem); cmd[4] = h_new(rst_ev); break;   /* GetHandles */
    case 0x0002: {                                                     /* Initialize(update period ms, raw C-stick) */
        static int ticking; uint32_t ms = cmd[1] ? cmd[1] : 10;
        rst_on = 1; if (!ticking++) k_add_periodic(rst_tick, (uint64_t)ms * 1000000ull);
        LOG(1, "ir:rst: ZL/ZR and C-stick every %u ms", ms); reply(cmd, 1, 0); break;
    }
    case 0x0003: rst_on = 0; reply(cmd, 1, 0); break;                  /* Shutdown */
    default: stub(s, cmd);
    }
}
/* ---- ir:USER: infrared accessories (Circle Pad Pro); none is attached ---- */
static void iruser(KObject *s, uint32_t *cmd) {
    static KObject *conn_ev, *send_ev, *recv_ev, *mem;
    if (!conn_ev) { conn_ev = k_event(0); send_ev = k_event(0); recv_ev = k_event(0); }
    switch (cmd[0] >> 16) {
    case 0x0018:                                                       /* InitializeIrNopShared(..., shared memory) */
        mem = h_get(cmd[8]);
        if (mem && mem->mem) { memset(mem->mem, 0, 16); mem->mem[0x0e] = 1; }
        reply(cmd, 1, 0); break;
    case 0x0006:                                                       /* RequireConnection: nothing answers */
        if (mem && mem->mem) { mem->mem[0x08] = 1; mem->mem[0x09] = 2; }
        reply(cmd, 1, 0); break;
    case 0x0009: if (mem && mem->mem) { mem->mem[0x08] = 0; mem->mem[0x0c] = 0; } reply(cmd, 1, 0); break;   /* Disconnect */
    case 0x0002: mem = 0; reply(cmd, 1, 0); break;                     /* FinalizeIrNop */
    case 0x000a: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(recv_ev); break;   /* GetReceiveEvent */
    case 0x000b: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(send_ev); break;   /* GetSendEvent */
    case 0x000c: reply(cmd, 1, 2); cmd[2] = 0; cmd[3] = h_new(conn_ev); break;   /* GetConnectionStatusEvent */
    case 0x000d: reply(cmd, 1, 0); cmd[1] = 0xc8a0000du; break;        /* SendIrNop: not connected (13, InvalidState; module not checked) */
    case 0x0019: reply(cmd, 1, 0); cmd[1] = 0xc88003efu; break;        /* ReleaseReceivedData: no data (NoData, NotFound; module not checked) */
    default: stub(s, cmd);
    }
}

/* ---- err:f: the game reports an unrecoverable error (a console would show the error screen) ---- */
static void errf(KObject *s, uint32_t *cmd) {
    if ((cmd[0] >> 16) == 0x0001)
        fatal("the game raised a fatal error: result %08x, raised at %08x (error type %u)", cmd[2], cmd[3], cmd[1] & 0xff);
    stub(s, cmd);
}

static const struct { const char *name; ServiceFn fn; } table[] = {
    { "srv:", srv }, { "err:f", errf },
    { "dsp::DSP", dsp }, { "y2r:u", y2r_service }, { "nfc:u", nfc_service }, { "nfc:m", nfc_service },
    { "hid:USER", hid }, { "hid:SPVR", hid }, { "gsp::Gpu", gsp_service },
    { "fs:USER", fs_user }, { "ndm:u", quiet_ok },
    { "ir:rst", irrst }, { "ir:USER", iruser },
    { "cecd:u", cecd }, { "boss:U", boss }, { "boss:P", boss }, { "frd:u", frd }, { "frd:a", frd }, { "ac:u", ac }, { "ac:i", ac },
    { "APT:A", apt }, { "APT:U", apt }, { "APT:S", apt },
    { "cfg:u", cfg }, { "cfg:s", cfg }, { "cfg:i", cfg },
    { "ptm:u", ptm }, { "ptm:s", ptm }, { "ptm:sysm", ptm }, { "ptm:gets", ptm }, { "ptm:play", ptm },
};
ServiceFn service_lookup(const char *name) {
    for (unsigned i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return stub;
}
void services_init(const char *dir) {
    game_dir = dir;
    nfc_set_dir(dir);
    romfs.size = romfs_open(dir);
    if (!romfs.size) fatal("cannot open %s/romfs.bin", dir);
    romfs.fixed = 1; romfs.virt = 1;
}
