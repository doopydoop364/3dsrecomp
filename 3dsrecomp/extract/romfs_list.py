#!/usr/bin/env python3
"""List files in an extracted RomFS (IVFC wrapper + level-3 directory tree)."""
import struct, sys, collections, os

p = sys.argv[1]
d = open(p, "rb")
hdr = d.read(0x60)
assert hdr[:4] == b"IVFC"
mhs = struct.unpack_from("<I", hdr, 8)[0]
bs3 = 1 << struct.unpack_from("<I", hdr, 0x4C)[0]
base = (0x60 + mhs + bs3 - 1) & ~(bs3 - 1)
d.seek(base); l3 = d.read(0x28)
(hl, dho, dhs, dmo, dms, fho, fhs, fmo, fms, fdo) = struct.unpack("<10I", l3)
assert hl == 0x28, f"level3 header length {hl:#x} at {base:#x}"
d.seek(base + dmo); dirs = d.read(dms)
d.seek(base + fmo); files = d.read(fms)

def dname(off):
    parent, sib, cdir, cfile, nxt, nl = struct.unpack_from("<6I", dirs, off)
    return parent, sib, cdir, cfile, dirs[off + 24:off + 24 + nl].decode("utf-16le")

def walk(off, path, out):
    while off != 0xFFFFFFFF:
        parent, sib, cdir, cfile, name = dname(off)
        full = path + ("/" if path else "") + name if off else ""
        f = cfile
        while f != 0xFFFFFFFF:
            fp, fs, dof, dsz, fn, nl = struct.unpack_from("<2I2Q2I", files, f)
            nm = files[f + 32:f + 32 + nl].decode("utf-16le")
            out.append(((full + "/" if full else "") + nm, dsz, fdo + dof))
            f = fs
        if cdir != 0xFFFFFFFF:
            walk(cdir, full, out)
        if off == 0:
            break
        off = sib

out = []
walk(0, "", out)
print(f"{len(out)} files, {sum(s for _, s, _ in out)/1e6:.1f} MB total (level3 at {base:#x})")
ext = collections.Counter(); extsz = collections.Counter(); top = collections.Counter()
for n, s, o in out:
    e = os.path.splitext(n)[1].lower() or "(none)"
    ext[e] += 1; extsz[e] += s
    top[n.split("/")[0]] += s
print("\nBy extension (count, MB):")
for e, c in ext.most_common(25):
    print(f"  {e:10s} {c:6d} {extsz[e]/1e6:8.1f}")
print("\nTop-level dirs (MB):")
for t, s in top.most_common(20):
    print(f"  {t:20s} {s/1e6:8.1f}")
print("\nSample:")
for n, s, o in out[:15]:
    print(f"  {n}  {s}")
with open(sys.argv[2], "w") as fh:
    for n, s, o in out:
        fh.write(f"{o}\t{s}\t{n}\n")
