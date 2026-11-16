#!/usr/bin/env python3
"""Decode a PICA200 RGBA8 render target from a VRAM dump into a landscape PNG (as shown on screen).
Usage: dump_framebuffer.py <vram.bin> <offset hex> <width> <height> <out.png>   (width/height as in the GPU register, e.g. 240 400)"""
import struct, sys, zlib
XL = [0, 1, 4, 5, 16, 17, 20, 21]; YL = [0, 2, 8, 10, 32, 34, 40, 42]
fn, off, W, H, out = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
d = open(fn, 'rb').read()[off:]
def px(x, y):                                   # framebuffer coordinates as the rasteriser uses them
    yy = (H - 1) - y
    o = (XL[x & 7] + YL[yy & 7] + (x & ~7) * 8) * 4 + (yy & ~7) * W * 4
    return d[o + 3], d[o + 2], d[o + 1]
# landscape image: the rasteriser's y (0..H-1) is horizontal, its x (0..W-1) is vertical (x+ = up)
img = bytearray(H * W * 3)
for ly in range(W):
    for lx in range(H):
        r, g, b = px(W - 1 - ly, lx)
        o = (ly * H + lx) * 3; img[o:o + 3] = bytes((r, g, b))
raw = b''.join(b'\0' + bytes(img[y * H * 3:(y + 1) * H * 3]) for y in range(W))
def chunk(t, data): return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', H, W, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
