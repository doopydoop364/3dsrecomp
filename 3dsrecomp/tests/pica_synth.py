#!/usr/bin/env python3
"""Synthetic PICA200 tests: build a command list, vertex data and shaders from scratch, run them through the
renderer (build/gpu_replay, software or Vulkan) and check which pixels were drawn. For GPU features no library game
uses (roadmap stage B: the whole PICA200 is implemented whether or not the 8 games need it), the expected result
comes from Azahar's behaviour, written down next to each test.

    tests/pica_synth.py [test name ...]      (default: all; RB_RENDERER=vulkan to check the Vulkan renderer)

Coverage is checked with RB_FLAT (every triangle of a draw gets one flat colour), so a test only says where
triangles landed; colour tests use the real fragment pipeline."""
import os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
W = H = 256                                     # render target 256x256 RGBA8 at the start of VRAM


def f24(x):
    """float -> PICA float24 (1 sign, 7 exponent bits biased by 63, 16 mantissa bits)."""
    if x == 0:
        return 0
    b = struct.unpack('<I', struct.pack('<f', x))[0]
    sign, exp, man = b >> 31, ((b >> 23) & 0xff) - 127 + 63, (b >> 7) & 0xffff
    return sign << 23 | (exp & 0x7f) << 16 | man


# ---- shader assembly (just what the tests need) ----
IDENT = 0x1B                                    # swizzle xyzw
def desc(mask=0xF, swz1=IDENT): return mask | swz1 << 5
def mov(dst, src, d=0): return 0x13 << 26 | dst << 21 | src << 12 | d
def end(): return 0x22 << 26
def emit(): return 0x2A << 26
def setemit(vid, prim=0, winding=0): return 0x2B << 26 | vid << 24 | prim << 23 | winding << 22
O = lambda n: n                                 # output register o<n>
V = lambda n: n                                 # input register v<n>
C = lambda n: 0x20 + n                          # float uniform c<n>


class List:
    def __init__(self):
        self.w = []
    def reg(self, id, *vals, mask=0xF):
        self.w += [vals[0], id | mask << 16 | (len(vals) - 1) << 20 | (1 << 31 if len(vals) > 1 else 0)]
        self.w += vals[1:]
        if len(self.w) & 1:
            self.w.append(0)
    def bytes(self):
        return struct.pack(f'<{len(self.w)}I', *self.w)


def setup(cl, vs_code, gs_code=None, nout=1):
    """Render target, viewport, write masks, the vertex shader (v0 = attribute 0, a float4 at 16-byte stride),
    and optionally a geometry shader; outputs: o0 = position."""
    cl.reg(0x11d, 0x18000000 >> 3)                         # colour buffer: VRAM start
    cl.reg(0x11c, 0)                                       # no depth buffer
    cl.reg(0x117, 0)                                       # RGBA8
    cl.reg(0x11e, W | (H - 1) << 12)
    cl.reg(0x041, f24(W / 2)); cl.reg(0x043, f24(H / 2)); cl.reg(0x068, 0)
    cl.reg(0x107, 0xF00); cl.reg(0x113, 0xF); cl.reg(0x115, 0)
    cl.reg(0x04f, nout); cl.reg(0x050, 0x03020100)         # o0 -> position
    cl.reg(0x2cb, 0); [cl.reg(0x2cc, w) for w in vs_code]
    cl.reg(0x2d5, 0); cl.reg(0x2d6, desc())
    cl.reg(0x2ba, 0); cl.reg(0x2bd, 1); cl.reg(0x2b9, 0); cl.reg(0x2bb, 0); cl.reg(0x2bc, 0)
    cl.reg(0x24a, nout - 1); cl.reg(0x251, nout - 1)
    cl.reg(0x201, 0xF); cl.reg(0x202, 0)                   # attribute 0: 4 floats; 1 attribute
    cl.reg(0x203, 0); cl.reg(0x204, 0); cl.reg(0x205, 1 << 28 | 16 << 16)
    cl.reg(0x25e, 0)
    if gs_code:
        cl.reg(0x29b, 0); [cl.reg(0x29c, w) for w in gs_code]
        cl.reg(0x2a5, 0); cl.reg(0x2a6, desc())
        cl.reg(0x28a, 0); cl.reg(0x28d, 1)


def tri(x0, y0, x1, y1, x2, y2):
    """Clip-space positions (z = -0.5, w = 1) of a triangle given in pixels."""
    return [((x / W) * 2 - 1, (y / H) * 2 - 1, -0.5, 1.0) for x, y in ((x0, y0), (x1, y1), (x2, y2))]


def run(name, cl, verts, idx=None):
    """Lay out: list at 0x30000000, vertices at +0x10000, indices at +0x20000; draw; returns the RGBA8 target."""
    d = tempfile.mkdtemp(prefix='pica_synth_')
    lin = bytearray(0x40000)
    vb = b''.join(struct.pack('<4f', *v) for v in verts)
    lin[0x10000:0x10000 + len(vb)] = vb
    if idx is not None:
        ib = struct.pack(f'<{len(idx)}H', *idx)
        lin[0x20000:0x20000 + len(ib)] = ib
    cl.reg(0x200, (0x20000000 + 0x10000) >> 3)             # attribute base (physical / 8)
    if idx is not None:
        cl.reg(0x227, (0x20000 - 0x10000) | 1 << 31)       # 16-bit indices, offset from the base
        cl.reg(0x228, len(idx)); cl.reg(0x22f, 1)
    else:
        cl.reg(0x228, len(verts)); cl.reg(0x22a, 0); cl.reg(0x22e, 1)
    lb = cl.bytes()
    lin[0:len(lb)] = lb
    open(f'{d}/linear.bin', 'wb').write(lin)
    open(f'{d}/vram.bin', 'wb').write(bytes(0x600000))
    env = dict(os.environ, RB_FLAT='1', RB_RENDERER=os.environ.get('RB_RENDERER', 'software'))
    r = subprocess.run([os.path.join(ROOT, 'build', 'gpu_replay'), '-', f'{d}/vram.bin', f'{d}/linear.bin', '30000000',
                        f'{len(lb):x}', f'{d}/out.bin'], env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{name}: gpu_replay failed:\n{r.stderr[-2000:]}')
    out = open(f'{d}/out.bin', 'rb').read()[:W * H * 4]
    return out, r.stderr


def drawn(img, x, y):
    """Was pixel (x, y) (y up, as the PICA counts) written?"""
    xl, yl = (0, 1, 4, 5, 16, 17, 20, 21), (0, 2, 8, 10, 32, 34, 40, 42)
    yy = H - 1 - y
    o = (xl[x & 7] + yl[yy & 7] + (x & ~7) * 8) * 4 + (yy & ~7) * W * 4
    return img[o:o + 4] != b'\0\0\0\0'


def expect(name, img, inside, outside):
    bad = [p for p in inside if not drawn(img, *p)] + [p for p in outside if drawn(img, *p)]
    print(f'{"ok  " if not bad else "FAIL"} {name}' + (f': wrong pixels {bad[:6]}' if bad else ''))
    return not bad


# ---- tests ----
T1 = tri(20, 20, 100, 20, 20, 100)              # left triangle
T2 = tri(150, 150, 230, 150, 150, 230)          # right triangle
IN = [(30, 30), (160, 160)]                     # inside T1, inside T2
OUT = [(90, 90), (220, 220), (128, 10), (10, 128)]


def gs_emit3(first):
    """GS program: emit c[first], c[first+1], c[first+2] as one triangle."""
    return [setemit(0), mov(O(0), C(first)), emit(), setemit(1), mov(O(0), C(first + 1)), emit(),
            setemit(2, prim=1), mov(O(0), C(first + 2)), emit(), end()]


def test_gs_fixed():
    """Fixed-primitive mode (0x252 mode 2): 3 vertices x 1 output fill c4..c6 (start 4), one invocation per 3."""
    cl = List()
    setup(cl, [mov(O(0), V(0)), end()], gs_emit3(4))
    cl.reg(0x229, 2)                                       # geometry shader on
    cl.reg(0x289, 0 | 1 << 8 | 0x08 << 24)                 # inputs to uniforms, GS mode
    cl.reg(0x252, 2 | (3 - 1) << 8 | (1 - 1) << 12 | 4 << 16)
    img, log = run('gs fixed', cl, T1 + T2)
    return expect('geometry shader, fixed-primitive mode', img, IN, OUT)


def test_gs_variable():
    """Variable-primitive mode (0x252 mode 1): an index is a vertex count n (c0 = n), then n vertices fill c1..;
    the first 0x254 + 1 with all outputs. Indices: [3, 0 1 2, 3, 3 4 5] -> two triangles."""
    cl = List()
    setup(cl, [mov(O(0), V(0)), end()], gs_emit3(1))
    cl.reg(0x229, 2 | 1 << 31)
    cl.reg(0x289, 0 | 1 << 8 | 0x08 << 24)
    cl.reg(0x252, 1); cl.reg(0x254, 3 - 1)
    img, log = run('gs variable', cl, T1 + T2, idx=[3, 0, 1, 2, 3, 3, 4, 5])
    return expect('geometry shader, variable-primitive mode', img, IN, OUT)


def test_no_gs():
    """Baseline: the same two triangles without a geometry shader."""
    cl = List()
    setup(cl, [mov(O(0), V(0)), end()])
    img, log = run('no gs', cl, T1 + T2)
    return expect('plain triangles (no geometry shader)', img, IN, OUT)


TESTS = {n[5:]: f for n, f in globals().items() if n.startswith('test_')}

if __name__ == '__main__':
    names = sys.argv[1:] or list(TESTS)
    ok = all([TESTS[n]() for n in names])
    sys.exit(0 if ok else 1)
