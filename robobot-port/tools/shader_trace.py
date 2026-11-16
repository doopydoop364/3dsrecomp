#!/usr/bin/env python3
"""Disassemble and trace a PICA200 vertex shader dumped by RB_PICA_DUMP (build/shader_dump.bin)."""
import struct, sys
d = open(sys.argv[1] if len(sys.argv) > 1 else 'build/shader_dump.bin', 'rb').read()
hdr = struct.unpack_from('<8I', d, 0); o = 32
code = struct.unpack_from('<512I', d, o); o += 2048
desc = struct.unpack_from('<128I', d, o); o += 512
uni = [struct.unpack_from('<4f', d, o + 16 * i) for i in range(96)]; o += 96 * 16
vin = [struct.unpack_from('<4f', d, o + 16 * i) for i in range(16)]; o += 256
sem = struct.unpack_from('<7I', d, o)
entry, bools, ints, outmask, nout = hdr[0], hdr[1], hdr[2:6], hdr[6], hdr[7]
NAMES = {0: 'add', 1: 'dp3', 2: 'dp4', 3: 'dph', 5: 'ex2', 6: 'lg2', 8: 'mul', 9: 'sge', 0xa: 'slt', 0xb: 'flr', 0xc: 'max',
         0xd: 'min', 0xe: 'rcp', 0xf: 'rsq', 0x12: 'mova', 0x13: 'mov', 0x18: 'dphi', 0x1a: 'sgei', 0x1b: 'slti', 0x20: 'break',
         0x21: 'nop', 0x22: 'end', 0x23: 'breakc', 0x24: 'call', 0x25: 'callc', 0x26: 'callu', 0x27: 'ifu', 0x28: 'ifc',
         0x29: 'loop', 0x2a: 'emit', 0x2b: 'setemit', 0x2c: 'jmpc', 0x2d: 'jmpu'}
def rn(r): return f'v{r}' if r < 16 else f'r{r - 16}' if r < 32 else f'c{r - 32}'
def sw(s): return ''.join('xyzw'[(s >> (6 - 2 * i)) & 3] for i in range(4))
def src(raw, s, neg, rel=0): return ('-' if neg else '') + rn(raw) + (['', '[a0.x]', '[a0.y]', '[aL]'][rel] if raw >= 32 else '') + '.' + sw(s)
def dm(m): return ''.join('xyzw'[i] for i in range(4) if m & (8 >> i))
def dis(pc):
    w = code[pc]; op = w >> 26
    if op < 0x20 or (op & ~1) == 0x2e:
        inv = 0x18 <= op <= 0x1b; dd = desc[w & 0x7f]; rel = (w >> 19) & 3
        s1 = (w >> 14) & 0x1f if inv else (w >> 12) & 0x7f; s2 = (w >> 7) & 0x7f if inv else (w >> 7) & 0x1f
        a = src(s1, (dd >> 5) & 0xff, (dd >> 4) & 1, 0 if inv else rel); b = src(s2, (dd >> 14) & 0xff, (dd >> 13) & 1, rel if inv else 0)
        if (op & ~1) == 0x2e: return f'cmp {a}, {b}  (x op {(w >> 24) & 7}, y op {(w >> 21) & 7})'
        dest = (w >> 21) & 0x1f
        return f'{NAMES.get(op, hex(op))} {"o" + str(dest) if dest < 16 else "r" + str(dest - 16)}.{dm(dd & 15)}, {a}, {b}'
    if op >= 0x30:
        madi = (op & ~7) == 0x30; dd = desc[w & 0x1f]; rel = (w >> 22) & 3; dest = (w >> 24) & 0x1f
        s2 = (w >> 12) & 0x1f if madi else (w >> 10) & 0x7f; s3 = (w >> 5) & 0x7f if madi else (w >> 5) & 0x1f
        return (f'{"madi" if madi else "mad"} {"o" + str(dest) if dest < 16 else "r" + str(dest - 16)}.{dm(dd & 15)}, '
                f'{src((w >> 17) & 0x1f, (dd >> 5) & 0xff, (dd >> 4) & 1)}, {src(s2, (dd >> 14) & 0xff, (dd >> 13) & 1, 0 if madi else rel)}, '
                f'{src(s3, (dd >> 23) & 0xff, (dd >> 22) & 1, rel if madi else 0)}')
    return f'{NAMES.get(op, hex(op))} target {(w >> 10) & 0xfff:03x} count {w & 0xff} cond {(w >> 22) & 15:x} refx {(w >> 25) & 1} refy {(w >> 24) & 1}'
print(f'entry {entry:03x} bools {bools:04x} ints {[hex(i) for i in ints]} outmask {outmask:04x} outputs {nout} semantics {[hex(x) for x in sem[:nout]]}')
for i, v in enumerate(vin):
    if any(v): print(f'  v{i} = {v}')
pc = entry
end = None
for k in range(80):
    print(f'{pc:03x}: {code[pc]:08x}  {dis(pc)}')
    if code[pc] >> 26 == 0x22: break
    pc += 1
