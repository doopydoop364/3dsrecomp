/* The game's RomFS, with an optional override folder.
 *
 * Games read their RomFS as one raw image and walk its level-3 directory and file tables themselves. To replace a
 * file, this keeps a patched copy of the file table in memory: an entry whose path exists under
 * <game folder>/romfs_override/ gets a new size and a data offset past the end of the original image, and reads of
 * that virtual range are served from the replacement file. Replacements may have any size; the original romfs.bin
 * is never modified. Directory layout (IVFC wrapper, level-3 header and tables) per 3dbrew. */
#define _GNU_SOURCE
#include "host.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint64_t off, size; int fd; char path[300]; } Override;
static int fd = -1;
static uint64_t base, orig_size, total_size, fmeta_off, fmeta_size, fdata_off;
static uint8_t *fmeta, *dmeta;
static Override *ovr; static int novr;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void name_utf8(const uint8_t *p, uint32_t len, char *out, size_t n) {
    size_t k = 0;
    for (uint32_t i = 0; i + 1 < len && k + 1 < n; i += 2) { uint16_t c = (uint16_t)(p[i] | p[i + 1] << 8); out[k++] = c < 0x80 ? (char)c : '_'; }
    out[k] = 0;
}

static void add_override(uint32_t entry, const char *rel, const char *root) {
    char host[600]; struct stat st;
    snprintf(host, sizeof host, "%s/%s", root, rel);
    if (stat(host, &st) || !S_ISREG(st.st_mode)) return;
    int h = open(host, O_RDONLY);
    if (h < 0) return;
    ovr = realloc(ovr, (size_t)(novr + 1) * sizeof *ovr);
    Override *o = &ovr[novr++];
    o->off = (total_size + 15) & ~15ull; o->size = (uint64_t)st.st_size; o->fd = h;
    snprintf(o->path, sizeof o->path, "%s", rel);
    total_size = o->off + o->size;
    uint64_t rel_off = o->off - fdata_off;                       /* entry offsets are relative to the data region */
    memcpy(fmeta + entry + 8, &rel_off, 8); memcpy(fmeta + entry + 16, &o->size, 8);
    LOG(1, "romfs: %s replaced by the override folder (%llu bytes)", rel, (unsigned long long)o->size);
}
static void walk(uint32_t dir, const char *path, const char *root, int depth) {
    if (depth > 64 || dir + 24 > (uint32_t)0x7fffffff) return;
    for (uint32_t f = rd32(dmeta + dir + 12); f != 0xffffffffu && f + 32 <= fmeta_size; f = rd32(fmeta + f + 4)) {
        char nm[256], rel[600];
        name_utf8(fmeta + f + 32, rd32(fmeta + f + 28), nm, sizeof nm);
        snprintf(rel, sizeof rel, "%s%s%s", path, *path ? "/" : "", nm);
        add_override(f, rel, root);
    }
    for (uint32_t c = rd32(dmeta + dir + 8); c != 0xffffffffu; c = rd32(dmeta + c + 4)) {
        char nm[256], sub[600];
        name_utf8(dmeta + c + 24, rd32(dmeta + c + 20), nm, sizeof nm);
        snprintf(sub, sizeof sub, "%s%s%s", path, *path ? "/" : "", nm);
        walk(c, sub, root, depth + 1);
    }
}

/* Opens <dir>/romfs.bin. Returns the size of the (virtual) level-3 image, or 0 on failure. */
uint64_t romfs_open(const char *dir) {
    char path[512]; struct stat st;
    snprintf(path, sizeof path, "%s/romfs.bin", dir);
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st)) return 0;
    uint8_t h[0x60];
    base = 0x1000;
    if (pread(fd, h, sizeof h, 0) == (ssize_t)sizeof h && !memcmp(h, "IVFC", 4)) {   /* level 3 follows the hash header */
        uint32_t mhs = rd32(h + 8), bs3 = 1u << rd32(h + 0x4c);
        base = (0x60 + mhs + bs3 - 1) & ~(uint64_t)(bs3 - 1);
    }
    orig_size = total_size = (uint64_t)st.st_size - base;
    uint8_t l3[0x28];
    if (pread(fd, l3, sizeof l3, (off_t)base) != (ssize_t)sizeof l3 || rd32(l3) != 0x28) return orig_size;
    uint32_t dmo = rd32(l3 + 12), dms = rd32(l3 + 16);
    fmeta_off = rd32(l3 + 28); fmeta_size = rd32(l3 + 32); fdata_off = rd32(l3 + 36);
    char root[600];
    snprintf(root, sizeof root, "%s/romfs_override", dir);
    if (stat(root, &st) || !S_ISDIR(st.st_mode)) return orig_size;
    dmeta = malloc(dms); fmeta = malloc(fmeta_size);
    if (!dmeta || !fmeta || pread(fd, dmeta, dms, (off_t)(base + dmo)) != (ssize_t)dms ||
        pread(fd, fmeta, fmeta_size, (off_t)(base + fmeta_off)) != (ssize_t)fmeta_size) { free(fmeta); fmeta = 0; return orig_size; }
    walk(0, "", root, 0);
    free(dmeta); dmeta = 0;
    if (!novr) { free(fmeta); fmeta = 0; }
    LOG(1, "romfs: %d file(s) replaced from %s", novr, root);
    return total_size;
}

/* Reads level-3 bytes [off, off+size) of the virtual image: patched file table, replacement files, original. */
int64_t romfs_read(uint64_t off, uint32_t size, uint8_t *dst) {
    if (fd < 0) return -1;
    if (off >= total_size) return 0;
    if (size > total_size - off) size = (uint32_t)(total_size - off);
    uint64_t done = 0;
    while (done < size) {
        uint64_t p = off + done, n = size - done;
        if (fmeta && p >= fmeta_off && p < fmeta_off + fmeta_size) {
            if (n > fmeta_off + fmeta_size - p) n = fmeta_off + fmeta_size - p;
            memcpy(dst + done, fmeta + (p - fmeta_off), n);
        } else if (p >= orig_size) {
            Override *o = 0;
            for (int i = 0; i < novr; i++) if (p >= ovr[i].off && p < ovr[i].off + ovr[i].size) o = &ovr[i];
            if (!o) {                                             /* alignment gap between replacements */
                uint64_t next = total_size;
                for (int i = 0; i < novr; i++) if (ovr[i].off > p && ovr[i].off < next) next = ovr[i].off;
                if (n > next - p) n = next - p;
                memset(dst + done, 0, n);
            } else {
                if (n > o->off + o->size - p) n = o->off + o->size - p;
                if (pread(o->fd, dst + done, n, (off_t)(p - o->off)) != (ssize_t)n) return (int64_t)done;
            }
        } else {
            if (n > orig_size - p) n = orig_size - p;
            if (fmeta && p < fmeta_off && n > fmeta_off - p) n = fmeta_off - p;   /* stop at the patched table */
            ssize_t r = pread(fd, dst + done, n, (off_t)(base + p));
            if (r <= 0) return (int64_t)done;
            n = (uint64_t)r;
        }
        done += n;
    }
    return (int64_t)done;
}
