/* ldr:ro: loads CRO modules (relocatable code the game maps at an address it chooses) and links them with the main
 * program (described by static.crs) and with each other. The rules follow Azahar's cro_helper.cpp; this is our own
 * implementation. Module code itself is recompiled ahead of time (modules.c finds it by name); here only the
 * tables and the data words relocations write are handled, in guest memory, exactly as the console does. */
#include "host.h"
#include <sys/mman.h>

/* Relocations also write the main program's import slots, some of them inside its read-only code pages (the
 * console's loader may): unprotect that page just for the write. */
static void wr32(uint32_t a, uint32_t v) {
    if (a - RECOMP_TEXT_BASE < RECOMP_RO_END - RECOMP_TEXT_BASE) {
        void *pg = (void *)(uintptr_t)(a & ~0xfffu);
        mprotect(pg, 0x2000, PROT_READ | PROT_WRITE); st32(a, v); mprotect(pg, 0x2000, PROT_READ);
    } else st32(a, v);
}

void modules_capture(uint32_t base, uint32_t size);    /* modules.c: keep the pristine module for the next build */
void modules_loaded(uint32_t base, uint32_t size);     /* modules.c: attach its recompiled code */
void modules_unloaded(uint32_t base);

#define HASH 0x80u
#define HEADER 0x138u
#define MAGIC_CRO0 0x304F5243u
#define MAGIC_FIXD 0x44584946u
enum { F_MAGIC, F_NAME, F_NEXT, F_PREV, F_FILESIZE, F_BSSSIZE, F_FIXEDSIZE, F_UNK0, F_UNKTAG, F_ONLOAD, F_ONEXIT, F_ONUNRESOLVED,
       F_CODE, F_CODESIZE, F_DATA, F_DATASIZE, F_MODNAME, F_MODNAMESIZE, F_SEG, F_SEGNUM, F_EXPNAMED, F_EXPNAMEDNUM,
       F_EXPIDX, F_EXPIDXNUM, F_EXPSTR, F_EXPSTRSIZE, F_EXPTREE, F_EXPTREENUM, F_IMPMOD, F_IMPMODNUM, F_EXTREL, F_EXTRELNUM,
       F_IMPNAMED, F_IMPNAMEDNUM, F_IMPIDX, F_IMPIDXNUM, F_IMPANON, F_IMPANONNUM, F_IMPSTR, F_IMPSTRSIZE,
       F_STATANON, F_STATANONNUM, F_INTREL, F_INTRELNUM, F_STATREL, F_STATRELNUM, F_FIX0 };
enum { FIX3 = F_EXPNAMED, FIX2 = F_IMPMOD, FIX1 = F_STATANON };
static const int fix_barrier[4] = { F_FIX0, FIX1, FIX2, FIX3 };
static const int entry_size[17] = { 1, 1, 1, 12, 8, 4, 1, 8, 20, 12, 8, 8, 8, 1, 8, 12, 12 };
enum { SEG_CODE, SEG_RODATA, SEG_DATA, SEG_BSS };

#define RO_ERR(d) (0xD9012C00u | (d))                 /* RO module, invalid state-ish; the exact codes are not used */

static uint32_t fld(uint32_t m, int f) { return ld32(m + HASH + 4u * (uint32_t)f); }
static void set_fld(uint32_t m, int f, uint32_t v) { st32(m + HASH + 4u * (uint32_t)f, v); }
static uint32_t entry(uint32_t m, int table_field, uint32_t i, uint32_t size) { return fld(m, table_field) + i * size; }

static uint32_t tag_addr(uint32_t m, uint32_t tag) {          /* segment tag -> address (0 if invalid) */
    uint32_t idx = tag & 15, off = tag >> 4;
    if (idx >= fld(m, F_SEGNUM)) return 0;
    uint32_t e = entry(m, F_SEG, idx, 12);
    if (off >= ld32(e + 4)) return 0;
    return ld32(e) + off;
}
static int cstr_eq(uint32_t a, uint32_t limit, const char *s) {
    for (uint32_t i = 0; i < limit; i++) { char c = (char)ld8(a + i); if (c != s[i]) return 0; if (!c) return 1; }
    return 0;
}
static void cstr(uint32_t a, char *out, size_t n) { size_t i = 0; for (; i + 1 < n; i++) { out[i] = (char)ld8(a + i); if (!out[i]) break; } out[i] = 0; }
static void module_name(uint32_t m, char *out, size_t n) { cstr(fld(m, F_MODNAME), out, n); }

static void apply_reloc(uint32_t target, uint32_t type, uint32_t addend, uint32_t symbol, uint32_t future) {
    switch (type) {
    case 0: break;
    case 2: case 38: wr32(target, symbol + addend); break;                       /* absolute address */
    case 3: wr32(target, symbol + addend - future); break;                        /* relative address */
    default: { static int warned; if (!warned++) LOG(1, "ldr:ro: relocation type %u is not implemented", type); }
    }
}
static void clear_reloc(uint32_t target, uint32_t type) { if (type == 2 || type == 38 || type == 3) wr32(target, 0); }
/* A batch of 12-byte relocation entries in module m, applied with one symbol address; marks the batch resolved. */
static int apply_batch(uint32_t m, uint32_t batch, uint32_t symbol, int reset) {
    if (!symbol && !reset) return -1;
    for (uint32_t r = batch;; r += 12) {
        uint32_t target = tag_addr(m, ld32(r));
        if (!target) return -1;
        apply_reloc(target, ld8(r + 4), ld32(r + 8), symbol, target);
        if (ld8(r + 5)) break;
    }
    st8(batch + 6, reset ? 0 : 1);
    return 0;
}
static uint32_t on_unresolved(uint32_t m) { return tag_addr(m, fld(m, F_ONUNRESOLVED)); }

static uint32_t find_export(uint32_t m, const char *name) {   /* named export by the module's bit-test tree */
    if (!fld(m, F_EXPTREENUM)) return 0;
    size_t len = strlen(name);
    uint32_t tree = fld(m, F_EXPTREE);
    uint16_t next = (uint16_t)ld16(tree + 2);                 /* root's left child */
    uint32_t id;
    for (;;) {
        uint32_t e = tree + (uint32_t)(next & 0x7fff) * 8;
        if (next & 0x8000) { id = ld16(e + 6); break; }
        uint32_t tb = ld16(e), byte = tb >> 3, bit = tb & 7;
        if (byte >= len) next = (uint16_t)ld16(e + 2);
        else next = (uint16_t)(((uint8_t)name[byte] >> bit) & 1 ? ld16(e + 4) : ld16(e + 2));
    }
    if (id >= fld(m, F_EXPNAMEDNUM)) return 0;
    uint32_t se = entry(m, F_EXPNAMED, id, 8);
    if (!cstr_eq(ld32(se), fld(m, F_EXPSTRSIZE), name)) return 0;
    return tag_addr(m, ld32(se + 4));
}

/* ---- rebase: file offsets in the tables become addresses ---- */
static int rebase_header(uint32_t m, uint32_t size) {
    if (fld(m, F_MAGIC) != MAGIC_CRO0 || fld(m, F_NEXT) || fld(m, F_PREV) || fld(m, F_FIXEDSIZE) || fld(m, F_CODE) < HEADER) return -1;
    if (fld(m, F_NAME)) set_fld(m, F_NAME, fld(m, F_NAME) + m);
    for (int f = F_CODE; f < F_FIX0; f += 2) if (fld(m, f)) set_fld(m, f, fld(m, f) + m);
    for (int f = F_CODE, i = 0; f < F_FIX0; f += 2, i++)
        if (fld(m, f) + fld(m, f + 1) * (uint32_t)entry_size[i] > m + size) return -1;
    return 0;
}
static void unrebase_header(uint32_t m) {
    if (fld(m, F_NAME)) set_fld(m, F_NAME, fld(m, F_NAME) - m);
    for (int f = F_CODE; f < F_FIX0; f += 2) if (fld(m, f)) set_fld(m, f, fld(m, f) - m);
}
/* Returns the data segment's old (in-file) address, or 0 on error. */
static uint32_t rebase_segments(uint32_t m, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size, int *err) {
    uint32_t prev_data = 0;
    for (uint32_t i = 0; i < fld(m, F_SEGNUM); i++) {
        uint32_t e = entry(m, F_SEG, i, 12), off = ld32(e), sz = ld32(e + 4), type = ld32(e + 8);
        if (type == SEG_DATA) { if (sz) { if (sz > data_size) { *err = 1; return 0; } prev_data = off; off = data; } }
        else if (type == SEG_BSS) { if (sz) { if (sz > bss_size) { *err = 1; return 0; } off = bss; } }
        else if (off) { off += m; if (off > m + size) { *err = 1; return 0; } }
        st32(e, off);
    }
    return prev_data + m;
}
static void unrebase_segments(uint32_t m) {
    for (uint32_t i = 0; i < fld(m, F_SEGNUM); i++) {
        uint32_t e = entry(m, F_SEG, i, 12);
        if (ld32(e + 8) == SEG_BSS) st32(e, 0); else if (ld32(e)) st32(e, ld32(e) - m);
    }
}
/* Adds (sign 1) or removes (sign -1) the module address to the offset fields of the symbol tables. */
static void rebase_tables(uint32_t m, int sign) {
    uint32_t d = sign > 0 ? m : (uint32_t)-m;
    for (uint32_t i = 0; i < fld(m, F_EXPNAMEDNUM); i++) { uint32_t e = entry(m, F_EXPNAMED, i, 8); if (ld32(e)) st32(e, ld32(e) + d); }
    for (uint32_t i = 0; i < fld(m, F_IMPMODNUM); i++) {
        uint32_t e = entry(m, F_IMPMOD, i, 20);
        if (ld32(e)) st32(e, ld32(e) + d);
        if (ld32(e + 4)) st32(e + 4, ld32(e + 4) + d);
        if (ld32(e + 12)) st32(e + 12, ld32(e + 12) + d);
    }
    for (uint32_t i = 0; i < fld(m, F_IMPNAMEDNUM); i++) {
        uint32_t e = entry(m, F_IMPNAMED, i, 8);
        if (ld32(e)) st32(e, ld32(e) + d);
        if (ld32(e + 4)) st32(e + 4, ld32(e + 4) + d);
    }
    for (uint32_t i = 0; i < fld(m, F_IMPIDXNUM); i++) { uint32_t e = entry(m, F_IMPIDX, i, 8); if (ld32(e + 4)) st32(e + 4, ld32(e + 4) + d); }
    for (uint32_t i = 0; i < fld(m, F_IMPANONNUM); i++) { uint32_t e = entry(m, F_IMPANON, i, 8); if (ld32(e + 4)) st32(e + 4, ld32(e + 4) + d); }
}
static void reset_external(uint32_t m) {                      /* every import points at OnUnresolved until linked */
    uint32_t unres = on_unresolved(m), n = fld(m, F_EXTRELNUM);
    int begin = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = entry(m, F_EXTREL, i, 12), target = tag_addr(m, ld32(r));
        if (target) apply_reloc(target, ld8(r + 4), ld32(r + 8), unres, target);
        if (begin) st8(r + 6, 0);
        begin = ld8(r + 5) != 0;
    }
}
static void clear_external(uint32_t m) {
    uint32_t n = fld(m, F_EXTRELNUM); int begin = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = entry(m, F_EXTREL, i, 12), target = tag_addr(m, ld32(r));
        if (target) clear_reloc(target, ld8(r + 4));
        if (begin) st8(r + 6, 0);
        begin = ld8(r + 5) != 0;
    }
}
static void apply_static_to_crs(uint32_t m, uint32_t crs) {   /* symbols this module gives the main program */
    for (uint32_t i = 0; i < fld(m, F_STATANONNUM); i++) {
        uint32_t e = entry(m, F_STATANON, i, 8);
        apply_batch(crs, ld32(e + 4) + m, tag_addr(m, ld32(e)), 0);
    }
}
static void apply_internal(uint32_t m, uint32_t old_data) {
    for (uint32_t i = 0; i < fld(m, F_INTRELNUM); i++) {
        uint32_t r = entry(m, F_INTREL, i, 12), tag = ld32(r);
        uint32_t tb = tag_addr(m, tag), target = tb;
        if (!tb) continue;
        if (ld32(entry(m, F_SEG, tag & 15, 12) + 8) == SEG_DATA) target = old_data + (tag >> 4);
        uint32_t sym_seg = ld8(r + 5);
        if (sym_seg >= fld(m, F_SEGNUM)) continue;
        apply_reloc(target, ld8(r + 4), ld32(r + 8), ld32(entry(m, F_SEG, sym_seg, 12)), tb);
    }
}
static void clear_internal(uint32_t m) {
    for (uint32_t i = 0; i < fld(m, F_INTRELNUM); i++) {
        uint32_t r = entry(m, F_INTREL, i, 12), t = tag_addr(m, ld32(r));
        if (t) clear_reloc(t, ld8(r + 4));
    }
}

/* ---- linking (the module list starts at static.crs: next = auto-link list, prev = manual list) ---- */
static uint32_t crs_addr;
#define FOR_AUTOLINK(x) for (uint32_t x = crs_addr; x; x = fld(x, F_NEXT))
static void apply_exit_relocs(uint32_t m) {                   /* __aeabi_atexit -> the RO library's nnroAeabiAtexit_ */
    for (uint32_t i = 0; i < fld(m, F_IMPNAMEDNUM); i++) {
        uint32_t e = entry(m, F_IMPNAMED, i, 8);
        if (!cstr_eq(ld32(e), fld(m, F_IMPSTRSIZE), "__aeabi_atexit")) continue;
        FOR_AUTOLINK(s) { uint32_t a = find_export(s, "nnroAeabiAtexit_"); if (a) { apply_batch(m, ld32(e + 4), a, 0); break; } }
    }
}
static void import_named(uint32_t m) {
    for (uint32_t i = 0; i < fld(m, F_IMPNAMEDNUM); i++) {
        uint32_t e = entry(m, F_IMPNAMED, i, 8), batch = ld32(e + 4);
        if (ld8(batch + 6)) continue;
        char name[256]; cstr(ld32(e), name, sizeof name);
        FOR_AUTOLINK(s) { uint32_t a = find_export(s, name); if (a) { apply_batch(m, batch, a, 0); break; } }
    }
}
static void import_modules(uint32_t m) {
    for (uint32_t i = 0; i < fld(m, F_IMPMODNUM); i++) {
        uint32_t e = entry(m, F_IMPMOD, i, 20);
        char want[128]; cstr(ld32(e), want, sizeof want);
        FOR_AUTOLINK(s) {
            char have[128]; module_name(s, have, sizeof have);
            if (strcmp(want, have)) continue;
            for (uint32_t j = 0; j < ld32(e + 8); j++) {
                uint32_t im = ld32(e + 4) + j * 8, ex = entry(s, F_EXPIDX, ld32(im), 4);
                apply_batch(m, ld32(im + 4), tag_addr(s, ld32(ex)), 0);
            }
            for (uint32_t j = 0; j < ld32(e + 16); j++) {
                uint32_t im = ld32(e + 12) + j * 8;
                apply_batch(m, ld32(im + 4), tag_addr(s, ld32(im)), 0);
            }
            break;
        }
    }
}
static void export_to(uint32_t m, uint32_t t, int reset) {    /* m's exports into module t's imports (or undo) */
    uint32_t unres = on_unresolved(t);
    for (uint32_t i = 0; i < fld(t, F_IMPNAMEDNUM); i++) {
        uint32_t e = entry(t, F_IMPNAMED, i, 8), batch = ld32(e + 4);
        if ((ld8(batch + 6) != 0) != reset) continue;
        char name[256]; cstr(ld32(e), name, sizeof name);
        uint32_t a = find_export(m, name);
        if (a) apply_batch(t, batch, reset ? unres : a, reset);
    }
    char mine[128]; module_name(m, mine, sizeof mine);
    for (uint32_t i = 0; i < fld(t, F_IMPMODNUM); i++) {
        uint32_t e = entry(t, F_IMPMOD, i, 20);
        if (!cstr_eq(ld32(e), fld(t, F_IMPSTRSIZE), mine)) continue;
        for (uint32_t j = 0; j < ld32(e + 8); j++) {
            uint32_t im = ld32(e + 4) + j * 8;
            apply_batch(t, ld32(im + 4), reset ? unres : tag_addr(m, ld32(entry(m, F_EXPIDX, ld32(im), 4))), reset);
        }
        for (uint32_t j = 0; j < ld32(e + 16); j++) {
            uint32_t im = ld32(e + 12) + j * 8;
            apply_batch(t, ld32(im + 4), reset ? unres : tag_addr(m, ld32(im)), reset);
        }
    }
}
static void link(uint32_t m, int bug_fix) {
    uint32_t saved = 0;
    if (bug_fix && fld(m, F_SEGNUM) >= 3) {                   /* imports land in the in-file data while linking */
        uint32_t e = entry(m, F_SEG, 2, 12); saved = ld32(e); st32(e, fld(m, F_DATA));
    }
    import_named(m); import_modules(m);
    if (bug_fix && fld(m, F_SEGNUM) >= 3) st32(entry(m, F_SEG, 2, 12), saved);
    FOR_AUTOLINK(t) export_to(m, t, 0);
}
static void unlink_(uint32_t m) {
    uint32_t unres = on_unresolved(m);
    for (uint32_t i = 0; i < fld(m, F_IMPNAMEDNUM); i++) apply_batch(m, ld32(entry(m, F_IMPNAMED, i, 8) + 4), unres, 1);
    for (uint32_t i = 0; i < fld(m, F_IMPIDXNUM); i++) apply_batch(m, ld32(entry(m, F_IMPIDX, i, 8) + 4), unres, 1);
    for (uint32_t i = 0; i < fld(m, F_IMPANONNUM); i++) apply_batch(m, ld32(entry(m, F_IMPANON, i, 8) + 4), unres, 1);
    FOR_AUTOLINK(t) export_to(m, t, 1);
}
static void reg(uint32_t m, int auto_link) {
    uint32_t head = auto_link ? fld(crs_addr, F_NEXT) : fld(crs_addr, F_PREV);
    if (head) {
        uint32_t tail = fld(head, F_PREV);
        set_fld(m, F_PREV, tail); set_fld(tail, F_NEXT, m); set_fld(head, F_PREV, m);
    } else {
        set_fld(m, F_PREV, m);
        set_fld(crs_addr, auto_link ? F_NEXT : F_PREV, m);
    }
    set_fld(m, F_NEXT, 0);
}
static void unreg(uint32_t m) {
    uint32_t nh = fld(crs_addr, F_NEXT), ph = fld(crs_addr, F_PREV), next = fld(m, F_NEXT), prev = fld(m, F_PREV);
    if (m == nh || m == ph) {
        if (next) set_fld(next, F_PREV, prev);
        set_fld(crs_addr, m == ph ? F_PREV : F_NEXT, next);
    } else if (next) { set_fld(prev, F_NEXT, next); set_fld(next, F_PREV, prev); }
    else {
        set_fld(prev, F_NEXT, 0);
        if (nh && fld(nh, F_PREV) == m) set_fld(nh, F_PREV, prev);
        else if (ph && fld(ph, F_PREV) == m) set_fld(ph, F_PREV, prev);
    }
    set_fld(m, F_NEXT, 0); set_fld(m, F_PREV, 0);
}
static uint32_t fix(uint32_t m, uint32_t level) {
    uint32_t end = HEADER + m;
    if (fld(m, F_CODE) + fld(m, F_CODESIZE) > end) end = fld(m, F_CODE) + fld(m, F_CODESIZE);
    for (int f = F_MODNAME, i = 2; f != fix_barrier[level > 3 ? 0 : level]; f += 2, i++) {
        uint32_t e = fld(m, f) + fld(m, f + 1) * (uint32_t)entry_size[i];
        if (e > end) end = e;
    }
    if (level) {
        set_fld(m, F_MAGIC, MAGIC_FIXD);
        for (int f = fix_barrier[level > 3 ? 0 : level]; f < F_FIX0; f += 2) { set_fld(m, f, end); set_fld(m, f + 1, 0); }
    }
    uint32_t fixed = ((end + 0xfff) & ~0xfffu) - m;
    set_fld(m, F_FIXEDSIZE, fixed);
    return fixed;
}
static int rebase(uint32_t m, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size, int is_crs) {
    if (rebase_header(m, size)) return -1;
    uint32_t old_data = 0; int err = 0;
    if (!is_crs) { old_data = rebase_segments(m, size, data, data_size, bss, bss_size, &err); if (err) return -1; }
    rebase_tables(m, 1);
    reset_external(m);
    if (!is_crs) apply_static_to_crs(m, crs_addr);
    apply_internal(m, old_data);
    if (!is_crs) apply_exit_relocs(m);
    return 0;
}

/* ---- the service ---- */
static void reply(uint32_t *cmd, int normal, int translate, uint32_t result) {
    cmd[0] = (cmd[0] & 0xffff0000u) | ((uint32_t)normal << 6) | (uint32_t)translate;
    cmd[1] = result;
}
void ldr_ro_service(KObject *s, uint32_t *cmd) {
    (void)s;
    switch (cmd[0] >> 16) {
    case 0x0001: {                                                  /* Initialize(crs buffer, size, address) */
        uint32_t buf = cmd[1], size = cmd[2], addr = cmd[3];
        if (crs_addr) { reply(cmd, 1, 0, 0xD9612C02u); break; }
        if (buf != addr && mem_alias(addr, buf, size, 0)) { reply(cmd, 1, 0, RO_ERR(1)); break; }
        crs_addr = addr;
        set_fld(addr, F_NEXT, 0); set_fld(addr, F_PREV, 0);
        int r = rebase(addr, size, 0, 0, 0, 0, 1);
        LOG(1, "ldr:ro: static module (static.crs) at %08x%s", addr, r ? ": rebase failed" : "");
        reply(cmd, 1, 0, r ? RO_ERR(2) : RES_OK); break;
    }
    case 0x0002: case 0x0003: reply(cmd, 1, 0, RES_OK); break;      /* LoadCRR / UnloadCRR: hashes are not checked */
    case 0x0004: case 0x0009: {                                     /* LoadCRO, LoadCRO_New */
        uint32_t buf = cmd[1], addr = cmd[2], size = cmd[3], data = cmd[4], data_size = cmd[6], bss = cmd[7], bss_size = cmd[8];
        int auto_link = (int)(cmd[9] & 0xff); uint32_t level = cmd[10];
        int bug_fix = (cmd[0] >> 16) == 0x0009;
        if (!crs_addr || (addr & 0xfff) || (size & 0xfff)) { reply(cmd, 2, 0, RO_ERR(3)); cmd[2] = 0; break; }
        if (buf != addr && mem_alias(addr, buf, size, 0)) { reply(cmd, 2, 0, RO_ERR(4)); cmd[2] = 0; break; }
        modules_capture(addr, size);
        if (rebase(addr, size, data, data_size, bss, bss_size, 0)) {
            LOG(1, "ldr:ro: module at %08x does not rebase", addr);
            if (buf != addr) mem_alias(addr, buf, size, 1);
            reply(cmd, 2, 0, RO_ERR(5)); cmd[2] = 0; break;
        }
        link(addr, bug_fix);
        reg(addr, auto_link);
        uint32_t fixed = fix(addr, level);
        if (buf != addr && fixed < size) mem_alias(addr + fixed, buf + fixed, size - fixed, 1);   /* the unused tail goes back */
        char name[128]; module_name(addr, name, sizeof name);
        LOG(1, "ldr:ro: module \"%s\" loaded at %08x (%x bytes, fixed %x, data %08x+%x, bss %08x+%x)", name, addr, size, fixed, data, data_size, bss, bss_size);
        modules_loaded(addr, size);
        reply(cmd, 2, 0, RES_OK); cmd[2] = fixed; break;
    }
    case 0x0005: {                                                  /* UnloadCRO(address, zero, buffer) */
        uint32_t addr = cmd[1], buf = cmd[3];
        if (!crs_addr || (fld(addr, F_MAGIC) != MAGIC_CRO0 && fld(addr, F_MAGIC) != MAGIC_FIXD)) { reply(cmd, 1, 0, RO_ERR(6)); break; }
        uint32_t fixed = fld(addr, F_FIXEDSIZE);
        int is_fixed = fld(addr, F_MAGIC) == MAGIC_FIXD;
        char name[128]; module_name(addr, name, sizeof name);
        modules_unloaded(addr);
        unreg(addr); unlink_(addr);
        if (!is_fixed) { clear_external(addr); clear_internal(addr); }
        rebase_tables(addr, -1); unrebase_segments(addr);
        set_fld(addr, F_NEXT, 0); set_fld(addr, F_PREV, 0); set_fld(addr, F_FIXEDSIZE, 0);
        unrebase_header(addr);
        if (buf != addr) mem_alias(addr, buf, fixed, 1);
        LOG(1, "ldr:ro: module \"%s\" unloaded from %08x", name, addr);
        reply(cmd, 1, 0, RES_OK); break;
    }
    case 0x0006: link(cmd[1], 0); reply(cmd, 1, 0, RES_OK); break;  /* LinkCRO */
    case 0x0007: unlink_(cmd[1]); reply(cmd, 1, 0, RES_OK); break;  /* UnlinkCRO */
    case 0x0008:                                                    /* Shutdown */
        if (crs_addr) { rebase_tables(crs_addr, -1); unrebase_header(crs_addr); crs_addr = 0; }
        reply(cmd, 1, 0, RES_OK); break;
    default:
        LOG(1, "STUB ldr:ro    cmd %08x", cmd[0]); reply(cmd, 1, 0, RES_OK);
    }
}
