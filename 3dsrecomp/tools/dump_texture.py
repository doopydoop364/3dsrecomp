#!/usr/bin/env python3
"""Decode a PICA200 texture from a memory dump to PNG. Usage: dump_texture.py <file> <offset> <w> <h> <fmt> <out.png>
Formats: 0 RGBA8, 12 ETC1, 13 ETC1A4. Rows are written top-down as stored (row 0 = top of memory image)."""
import struct, sys, zlib
XL = [0, 1, 4, 5, 16, 17, 20, 21]; YL = [0, 2, 8, 10, 32, 34, 40, 42]
MOD = [(2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183)]
def x4(v): return (v << 4) | v
def x5(v): return (v << 3) | (v >> 2)
def etc1(blk, lx, ly):
    b = struct.unpack('<Q', blk)[0]
    texel = lx * 4 + ly; flip = (b >> 32) & 1; diff = (b >> 33) & 1
    second = ly >= 2 if flip else lx >= 2
    table = (b >> 34) & 7 if second else (b >> 37) & 7
    if diff:
        r, g, bl = (b >> 59) & 31, (b >> 51) & 31, (b >> 43) & 31
        if second:
            sx = lambda v: (v ^ 4) - 4
            r += sx((b >> 56) & 7); g += sx((b >> 48) & 7); bl += sx((b >> 40) & 7)
        base = [x5(r & 31), x5(g & 31), x5(bl & 31)]
    else:
        base = [x4((b >> (56 if second else 60)) & 15), x4((b >> (48 if second else 52)) & 15), x4((b >> (40 if second else 44)) & 15)]
    m = MOD[table][(b >> texel) & 1]
    if (b >> (16 + texel)) & 1: m = -m
    return [max(0, min(255, c + m)) for c in base]
def main():
    fn, off, w, h, fmt, out = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
    d = open(fn, 'rb').read()[off:]
    bpp = {0: 32, 12: 4, 13: 8}[fmt]
    tile = 8 * bpp
    img = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            tp = ((y >> 3) * (w >> 3) + (x >> 3)) * tile
            fx, fy = x & 7, y & 7
            if fmt in (12, 13):
                sub = (fx >> 2) + 2 * (fy >> 2); sz = 16 if fmt == 13 else 8
                blk = d[tp + sub * sz + (8 if fmt == 13 else 0):][:8]
                c = etc1(blk, fx & 3, fy & 3)
            else:
                p = tp + (XL[fx] + YL[fy]) * 4
                c = [d[p + 3], d[p + 2], d[p + 1]]
            img[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(c)
    raw = b''.join(b'\0' + bytes(img[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    def chunk(t, data): return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
main()
