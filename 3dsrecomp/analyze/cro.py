"""CRO / CRS modules (relocatable code loaded through ldr:ro): parsing for the build.

Field layout and rules as in runtime/ldr_ro.c (after Azahar's cro_helper.cpp). Offsets here are file offsets, i.e.
addresses in "offset space": the module as if it were loaded at address 0. static.crs describes the main program;
its segment table holds the main program's absolute addresses.
"""
import struct

HASH, HEADER = 0x80, 0x138
F = dict(MAGIC=0, NAME=1, CODE=12, CODESIZE=13, DATA=14, DATASIZE=15, MODNAME=16, MODNAMESIZE=17, SEG=18, SEGNUM=19,
         EXPNAMED=20, EXPNAMEDNUM=21, EXPIDX=22, EXPIDXNUM=23, EXPSTR=24, EXPSTRSIZE=25, IMPMOD=28, IMPMODNUM=29,
         EXTREL=30, EXTRELNUM=31, IMPNAMED=32, IMPNAMEDNUM=33, IMPANON=36, IMPANONNUM=37, STATANON=40, STATANONNUM=41,
         INTREL=42, INTRELNUM=43, STATREL=44, STATRELNUM=45, UNKTAG=8, ONLOAD=9, ONEXIT=10, ONUNRESOLVED=11)
SEG_CODE, SEG_RODATA, SEG_DATA, SEG_BSS = 0, 1, 2, 3


class Cro:
    def __init__(self, data, is_crs=False):
        self.b = bytes(data)
        self.is_crs = is_crs
        if self.f('MAGIC') not in (0x304F5243, 0x44584946):
            raise ValueError('not a CRO/CRS module')
        self.segs = [struct.unpack_from('<3I', self.b, self.f('SEG') + 12 * i) for i in range(self.f('SEGNUM'))]

    def f(self, name):
        return struct.unpack_from('<I', self.b, HASH + 4 * F[name])[0]

    def u32(self, off):
        return struct.unpack_from('<I', self.b, off)[0]

    def cstr(self, off):
        e = self.b.index(b'\0', off)
        return self.b[off:e].decode('latin-1')

    @property
    def name(self):
        return self.cstr(self.f('MODNAME'))

    def tag(self, t):
        """Segment tag -> address (offset space for a module, absolute for static.crs), or None."""
        idx, off = t & 15, t >> 4
        if idx >= len(self.segs) or off >= self.segs[idx][1]:
            return None
        return self.segs[idx][0] + off

    def code_range(self):
        for off, size, typ in self.segs:
            if typ == SEG_CODE and size:
                return off, off + size
        return 0, 0

    def relocations(self, table, count, size=12):
        for i in range(self.f(count)):
            o = self.f(table) + size * i
            yield struct.unpack_from('<IBBBBI', self.b, o)        # tag, type, byte5, byte6, pad, addend

    # ---- what the build needs ----
    def code_seeds(self):
        """Function keys (offset space, Thumb with bit 0) that are certainly code: exports, the load / exit /
        unresolved hooks, and internal relocations whose symbol lies in the code segment (vtables, callbacks)."""
        lo, hi = self.code_range()
        keys = set()
        def add(v):
            if v is not None and lo <= (v & ~1) < hi:
                keys.add(v)
        for i in range(self.f('EXPNAMEDNUM')):
            add(self.tag(self.u32(self.f('EXPNAMED') + 8 * i + 4)))
        for i in range(self.f('EXPIDXNUM')):
            add(self.tag(self.u32(self.f('EXPIDX') + 4 * i)))
        for name in ('ONLOAD', 'ONEXIT', 'ONUNRESOLVED', 'UNKTAG'):
            add(self.tag(self.f(name)))
        for i in range(self.f('STATANONNUM')):
            add(self.tag(self.u32(self.f('STATANON') + 8 * i)))
        for tag, typ, sym_seg, _, _, addend in self.relocations('INTREL', 'INTRELNUM'):
            if typ in (2, 38) and sym_seg < len(self.segs) and self.segs[sym_seg][2] == SEG_CODE:
                add(self.segs[sym_seg][0] + addend)
        return keys

    def offset_image(self):
        """The module bytes with its internal relocations applied as if loaded at 0 (data segment at its file place):
        literal pools and tables then hold offsets, which is what analysis needs."""
        img = bytearray(self.b)
        for tag, typ, sym_seg, _, _, addend in self.relocations('INTREL', 'INTRELNUM'):
            t = self.tag(tag)
            if t is None or sym_seg >= len(self.segs) or typ not in (2, 38, 3):
                continue
            if self.segs[tag & 15][2] == SEG_DATA:
                t = self.f('DATA') + (tag >> 4)
            v = self.segs[sym_seg][0] + addend
            if typ == 3:
                v -= t
            if 0 <= t <= len(img) - 4:
                struct.pack_into('<I', img, t, v & 0xFFFFFFFF)
        return bytes(img)

    def relocated_words(self):
        """Offsets of every word a relocation of this module writes in its own segments."""
        out = set()
        for table, count in (('INTREL', 'INTRELNUM'), ('EXTREL', 'EXTRELNUM')):
            for tag, *_ in self.relocations(table, count):
                t = self.tag(tag)
                if t is not None:
                    out.add(t)
        return out


def main_program_words(crs, modules):
    """Absolute addresses of main-program words written at run time: static.crs's import slots, and the words each
    module's static relocations write into the main program."""
    words = set()
    for tag, *_ in crs.relocations('EXTREL', 'EXTRELNUM'):
        t = crs.tag(tag)
        if t is not None:
            words.add(t)
    for m in modules:
        for tag, *_ in m.relocations('STATREL', 'STATRELNUM'):
            t = crs.tag(tag)
            if t is not None:
                words.add(t)
    return words


def main_program_seeds(crs):
    """Main-program functions modules reach through static.crs's exports."""
    lo, hi = crs.code_range()
    keys = set()
    for i in range(crs.f('EXPNAMEDNUM')):
        v = crs.tag(crs.u32(crs.f('EXPNAMED') + 8 * i + 4))
        if v is not None and lo <= (v & ~1) < hi:
            keys.add(v)
    for i in range(crs.f('EXPIDXNUM')):
        v = crs.tag(crs.u32(crs.f('EXPIDX') + 4 * i))
        if v is not None and lo <= (v & ~1) < hi:
            keys.add(v)
    return keys


def romfs_files(path, want):
    """(name, bytes) of the files in a RomFS image (IVFC-wrapped, as extracted) whose name `want(name)` accepts."""
    with open(path, 'rb') as d:
        hdr = d.read(0x60)
        mhs = struct.unpack_from('<I', hdr, 8)[0]
        bs3 = 1 << struct.unpack_from('<I', hdr, 0x4C)[0]
        base = (0x60 + mhs + bs3 - 1) & ~(bs3 - 1)
        d.seek(base)
        hl, _, _, dmo, dms, _, _, fmo, fms, fdo = struct.unpack('<10I', d.read(0x28))
        d.seek(base + dmo); dirs = d.read(dms)
        d.seek(base + fmo); files = d.read(fms)
        found = []

        def walk(off, path):
            while off != 0xFFFFFFFF:
                _, sib, cdir, cfile, _, nl = struct.unpack_from('<6I', dirs, off)
                name = dirs[off + 24:off + 24 + nl].decode('utf-16le')
                full = (path + '/' if path else '') + name if off else ''
                f = cfile
                while f != 0xFFFFFFFF:
                    _, fs, dof, dsz, _, fnl = struct.unpack_from('<2I2Q2I', files, f)
                    nm = files[f + 32:f + 32 + fnl].decode('utf-16le')
                    p = (full + '/' if full else '') + nm
                    if want(p):
                        found.append((p, base + fdo + dof, dsz))
                    f = fs
                if cdir != 0xFFFFFFFF:
                    walk(cdir, full)
                if off == 0:
                    break
                off = sib
        walk(0, '')
        out = []
        for p, o, n in found:
            d.seek(o)
            out.append((p, d.read(n)))
        return out
