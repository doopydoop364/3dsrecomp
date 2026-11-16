#!/usr/bin/env python3
"""Extract ExHeader, ExeFS files and raw RomFS from a decrypted CIA's first NCCH."""
import os, struct, sys

def align64(x): return (x + 63) & ~63
def u32(b, o=0): return struct.unpack_from("<I", b, o)[0]

def blz_decode(data):
    """Backwards LZ (BLZ) as used by compressed ExeFS .code."""
    enc_len = int.from_bytes(data[-8:-5], "little")
    hdr_len = data[-5]
    inc_len = u32(data, len(data) - 4)
    if enc_len == 0 and inc_len == 0:
        return bytes(data)
    raw = bytearray(data) + bytearray(inc_len)
    pos_in = len(data) - hdr_len
    pos_out = len(raw)
    stop = len(data) - enc_len
    while pos_in > stop and pos_out > 0:
        pos_in -= 1
        flags = raw[pos_in]
        for _ in range(8):
            if pos_in <= stop or pos_out <= 0:
                break
            if flags & 0x80:
                pos_in -= 1; b1 = raw[pos_in]
                pos_in -= 1; b0 = raw[pos_in]
                length = (b1 >> 4) + 3
                disp = (((b1 & 0xF) << 8) | b0) + 3
                for _ in range(length):
                    pos_out -= 1
                    raw[pos_out] = raw[pos_out + disp]
            else:
                pos_in -= 1; pos_out -= 1
                raw[pos_out] = raw[pos_in]
            flags = (flags << 1) & 0xFF
    return bytes(raw)

def main(cia, outdir):
    os.makedirs(outdir, exist_ok=False)
    with open(cia, "rb") as f:
        h = f.read(0x20)
        hs, cert, tik, tmd = u32(h, 0), u32(h, 8), u32(h, 0xC), u32(h, 0x10)
        content_off = align64(align64(align64(align64(hs) + cert) + tik) + tmd)
        base = content_off
        f.seek(base)
        ncch = f.read(0x200)
        assert ncch[0x100:0x104] == b"NCCH", "first content is not a plaintext NCCH"
        flags = ncch[0x188:0x190]
        print("NCCH flags:", flags.hex(), "(bit 0x04 of last byte = NoCrypto)")
        exh_len = u32(ncch, 0x180)
        exefs_off, exefs_len = u32(ncch, 0x1A0) * 0x200, u32(ncch, 0x1A4) * 0x200
        romfs_off, romfs_len = u32(ncch, 0x1B0) * 0x200, u32(ncch, 0x1B4) * 0x200
        print(f"title id {struct.unpack_from('<Q', ncch, 0x108)[0]:016X}  product {ncch[0x150:0x160].rstrip(bytes(1))!r}")
        print(f"exheader_len={exh_len:#x} exefs={exefs_off:#x}+{exefs_len:#x} romfs={romfs_off:#x}+{romfs_len:#x}")

        f.seek(base + 0x200); exh = f.read(0x800)
        open(os.path.join(outdir, "exheader.bin"), "wb").write(exh)
        compressed = bool(exh[0xD] & 1)
        text = (u32(exh, 0x10), u32(exh, 0x14), u32(exh, 0x18))  # addr, pages, size
        ro   = (u32(exh, 0x20), u32(exh, 0x24), u32(exh, 0x28))
        data = (u32(exh, 0x30), u32(exh, 0x34), u32(exh, 0x38))
        bss = u32(exh, 0x3C)
        print(f"name={exh[:8].rstrip(bytes(1))!r} code_compressed={compressed}")
        print(f".text {text[0]:#x} size {text[2]:#x} | .rodata {ro[0]:#x} size {ro[2]:#x} | .data {data[0]:#x} size {data[2]:#x} | bss {bss:#x}")

        f.seek(base + exefs_off); hdr = f.read(0x200)
        for i in range(10):
            e = hdr[i*16:(i+1)*16]
            name = e[:8].rstrip(bytes(1)).decode("ascii", "replace")
            off, size = u32(e, 8), u32(e, 12)
            if not name or not size: continue
            f.seek(base + exefs_off + 0x200 + off)
            blob = f.read(size)
            open(os.path.join(outdir, name.lstrip(".") + ".bin"), "wb").write(blob)
            print(f"ExeFS {name}: {size:#x} bytes")
            if name == ".code" and compressed:
                dec = blz_decode(blob)
                expect = text[2] + ro[2] + data[2]
                open(os.path.join(outdir, "code_decompressed.bin"), "wb").write(dec)
                print(f"  decompressed to {len(dec):#x}; exheader expects {expect:#x} -> {'MATCH' if len(dec)==expect else 'MISMATCH'}")

        if romfs_off:
            f.seek(base + romfs_off)
            print("RomFS magic:", f.read(4))
            f.seek(base + romfs_off)
            with open(os.path.join(outdir, "romfs.bin"), "wb") as o:
                left = romfs_len
                while left:
                    c = f.read(min(left, 8 << 20)); o.write(c); left -= len(c)
            print("RomFS saved")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
