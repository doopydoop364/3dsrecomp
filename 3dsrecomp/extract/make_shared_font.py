#!/usr/bin/env python3
"""Build the shared system font memory image the runtime hands to games (APT GetSharedFont).

Input: the RomFS of a shared font system archive dumped from the user's console (for USA/EUR/JPN
consoles that is title 0004009b00014002), or a .bcfnt / LZ11-compressed .bcfnt file directly.
Output: an 0x80-byte header (state 2 = loaded, region, size) followed by the font with its internal
offsets resolved to absolute addresses, as the system does on a console.

Usage: make_shared_font.py <archive.romfs | font.bcfnt[.lz]> <out shared_font.bin> [path inside the RomFS]

With a third argument the font is taken from that path of the RomFS. This allows a stand-in: a
game's own font from its RomFS can serve as the system font until the real archive is available.
"""
import struct, sys

FONT_ADDR = 0x18000000          # fixed address of the shared font block on every console
HEADER = 0x80


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def lz11(src):
    if src[0] != 0x11:
        raise ValueError('not LZ11 data')
    size, pos = int.from_bytes(src[1:4], 'little'), 4
    if size == 0:
        size, pos = u32(src, 4), 8
    out = bytearray()
    while len(out) < size:
        flags = src[pos]; pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(src[pos]); pos += 1
                continue
            b0 = src[pos]; kind = b0 >> 4
            if kind == 0:
                n = (((b0 & 0xF) << 4) | (src[pos + 1] >> 4)) + 0x11
                disp = (((src[pos + 1] & 0xF) << 8) | src[pos + 2]) + 1; pos += 3
            elif kind == 1:
                n = (((b0 & 0xF) << 12) | (src[pos + 1] << 4) | (src[pos + 2] >> 4)) + 0x111
                disp = (((src[pos + 2] & 0xF) << 8) | src[pos + 3]) + 1; pos += 4
            else:
                n = kind + 1
                disp = (((b0 & 0xF) << 8) | src[pos + 1]) + 1; pos += 2
            for _ in range(n):
                out.append(out[-disp])
    return bytes(out[:size])


def romfs_files(img):
    """Yield (path, data) for every file of a RomFS image (with or without the IVFC header)."""
    base = 0
    if img[:4] == b'IVFC':
        bs = 1 << u32(img, 0x4C)
        base = (0x60 + u32(img, 8) + bs - 1) & ~(bs - 1)
    hl, _, _, dmo, dms, _, _, fmo, fms, fdo = struct.unpack_from('<10I', img, base)
    if hl != 0x28:
        raise ValueError('not a RomFS image')
    dirs, files = img[base + dmo: base + dmo + dms], img[base + fmo: base + fmo + fms]

    def walk(off, path):
        _, sib, child, ffile, _, nl = struct.unpack_from('<6I', dirs, off)
        name = dirs[off + 24: off + 24 + nl].decode('utf-16le')
        full = f'{path}/{name}' if off else ''
        f = ffile
        while f != 0xFFFFFFFF:
            _, fs, dof, dsz, _, fnl = struct.unpack_from('<2I2Q2I', files, f)
            yield f'{full}/{files[f + 32: f + 32 + fnl].decode("utf-16le")}', img[base + fdo + dof: base + fdo + dof + dsz]
            f = fs
        if child != 0xFFFFFFFF:
            yield from walk(child, full)
        if off and sib != 0xFFFFFFFF:
            yield from walk(sib, path)
    yield from walk(0, '')


def resolve(font, base):
    """Turn file-relative section offsets into absolute addresses (CFNT -> CFNU)."""
    f = bytearray(font)
    if f[:4] not in (b'CFNT', b'CFNU'):
        raise ValueError(f'not a BCFNT font (magic {bytes(f[:4])!r})')
    if f[:4] == b'CFNU':
        raise ValueError('font is already resolved')
    finf = struct.unpack_from('<H', f, 6)[0]                 # header size: FINF follows the file header
    if f[finf:finf + 4] != b'FINF':
        raise ValueError('FINF section not found')

    def patch(pos):
        v = u32(f, pos)
        struct.pack_into('<I', f, pos, v + base if v else 0)
        return v
    tglp, cwdh, cmap = patch(finf + 0x10), patch(finf + 0x14), patch(finf + 0x18)
    if f[tglp - 8: tglp - 4] != b'TGLP':
        raise ValueError('TGLP section not found')
    patch(tglp - 8 + 0x1C)                                    # sheet data
    counts = {'CWDH': 0, 'CMAP': 0}
    for magic, off, nxt in ((b'CWDH', cwdh, 0x0C), (b'CMAP', cmap, 0x10)):
        while off:
            if f[off - 8: off - 4] != magic:
                raise ValueError(f'{magic.decode()} chain is broken at {off:#x}')
            counts[magic.decode()] += 1
            off = patch(off - 8 + nxt)
    f[:4] = b'CFNU'
    return bytes(f), counts


def main():
    src, out = sys.argv[1:3]
    data = open(src, 'rb').read()
    name = src
    if data[:4] == b'IVFC' or u32(data, 0) == 0x28:
        want = sys.argv[3].strip('/') if len(sys.argv) > 3 else None
        fonts = [(p, d) for p, d in romfs_files(data) if (p.strip('/') == want if want else '.bcfnt' in p.lower())]
        if not fonts:
            sys.exit('no .bcfnt file in this archive')
        name, data = fonts[0]
    if data[0] == 0x11 and data[:4] != b'CFNT':
        data = lz11(data)
    font, counts = resolve(data, FONT_ADDR + HEADER)
    image = struct.pack('<3I', 2, 1, len(font)).ljust(HEADER, b'\0') + font
    open(out, 'wb').write(image)
    print(f'{name}: {len(font)} bytes, {counts["CWDH"]} width tables, {counts["CMAP"]} character maps -> {out} ({len(image)} bytes)')


if __name__ == '__main__':
    main()
