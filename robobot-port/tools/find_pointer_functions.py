#!/usr/bin/env python3
"""Find code that is only reachable through stored pointers (vtables, callbacks, member pointers).

Every word of the image that is not a recompiled instruction, and whose value is the address of
cleanly decodable code in .text, is treated as a function pointer. Init-array ranges are detected
from the SDK start-up code (idioms.py).
Usage: find_pointer_functions.py <image> <gen dir> <out.txt>
"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
import armrecomp as R, idioms, layout
img, gen, out = sys.argv[1:4]
d = open(img, 'rb').read()
lay = layout.read_layout(os.path.join(os.path.dirname(img), 'exheader.bin'))
nonlocal_returns = idioms.detect_inline_data_calls(d, lay.text_base, lay.text_base, lay.text_end)
R.configure(d, lay, nonlocal_returns)
cov = open(f'{gen}/covered.bin', 'rb').read()
known = {int(l, 16) for l in open(f'{gen}/functions_all.txt')}
cand = set()
for off in range(0, len(d) - 3, 4):
    if off // 4 < len(cov) and cov[off // 4]:
        continue                                   # this word is an instruction, not data
    v = struct.unpack_from('<I', d, off)[0]
    if R.in_text(v) and v not in known and v not in cand and R.is_clean_code(v, known):
        cand.add(v)
# C++ static initialisers: tables of self-relative offsets (entry address + stored value = constructor)
n_init = 0
for lo, hi in idioms.detect_relative_init_arrays(d, lay.text_base, lay.text_base, lay.text_end):
    for a in range(lo, hi, 4):
        v = (a + struct.unpack_from('<I', d, a - lay.text_base)[0]) & 0xFFFFFFFF
        if R.in_text(v) and v not in known and v not in cand:
            cand.add(v); n_init += 1
print(f'{n_init} static-initialiser entries from the init array')

def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v

# Self-relative jump tables: `add/sub rX, pc, #imm` pointing at a run of negative offsets; each
# entry + table address is a code entry point (hand-written routines dispatch through them).
n_tab = 0
T0, T1 = lay.text_base, lay.text_end
for off in range(0, T1 - T0, 4):
    w = struct.unpack_from('<I', d, off)[0]
    if (w & 0x0FFF0000) not in (0x024F0000, 0x028F0000):
        continue
    a = T0 + off
    imm = ror(w & 0xFF, ((w >> 8) & 0xF) * 2)
    t = a + 8 - imm if (w & 0x0FFF0000) == 0x024F0000 else a + 8 + imm
    if not (T0 <= t < T1) or t & 3:
        continue
    entries = []
    for k in range(64):
        v = struct.unpack_from('<i', d, t - T0 + 4 * k)[0]
        if not (-0x4000 < v < 0) or (t + v) & 3:
            break
        entries.append(t + v)
    if len(entries) >= 2:
        # the routine that owns the table often starts right after it, at the `sub rX, pc` itself
        if a == t + 4 * len(entries) and a not in known and a not in cand and R.is_clean_code(a, known):
            entries.append(a)
        for e in entries:
            if R.in_text(e) and e not in known and e not in cand and R.is_clean_code(e, known):
                cand.add(e); n_tab += 1
print(f'{n_tab} entries from self-relative jump tables')

# Targets of `add pc, rX, rTable` jump tables (positive self-relative offsets): hand-written code also
# reaches them from other functions and through hand-made return addresses.
n_pt = 0
for off in range(0, T1 - T0, 4):
    w = struct.unpack_from('<I', d, off)[0]
    if (w & 0x0FF0FFF0) != 0x0080F000 or (w >> 28) != 0xE:
        continue
    ins = R.Ins(T0 + off, w)
    tab = R.selfrel_table(ins)
    for t in (tab[1] if tab else []):
        if t not in known and t not in cand and R.is_clean_code(t, known):
            cand.add(t); n_pt += 1
print(f'{n_pt} targets of add-pc jump tables')

# Code addresses built in two steps: `add/sub rX, pc, #a` then `add/sub rX, rX, #b` (hand-made return
# addresses and jump targets in hand-written code).
n_two = 0
for off in range(0, T1 - T0 - 16, 4):
    w = struct.unpack_from('<I', d, off)[0]
    if (w & 0x0FEF0000) not in (0x028F0000, 0x024F0000) or (w >> 28) != 0xE:
        continue
    a = T0 + off
    rd = (w >> 12) & 0xF
    imm = ror(w & 0xFF, ((w >> 8) & 0xF) * 2)
    v = a + 8 + imm if (w & 0x00F00000) == 0x00800000 else a + 8 - imm
    for k in (1, 2, 3):
        w2 = struct.unpack_from('<I', d, off + 4 * k)[0]
        if (w2 & 0x0FE00000) in (0x02800000, 0x02400000) and ((w2 >> 16) & 0xF) == rd and ((w2 >> 12) & 0xF) == rd:
            imm2 = ror(w2 & 0xFF, ((w2 >> 8) & 0xF) * 2)
            t = (v + imm2 if (w2 & 0x00E00000) == 0x00800000 else v - imm2) & 0xFFFFFFFF
            if R.in_text(t) and t not in known and t not in cand and R.is_clean_code(t, known):
                cand.add(t); n_two += 1
            break
        if ((w2 >> 12) & 0xF) == rd:
            break
print(f'{n_two} code addresses built in two steps')

# Function starts the other scans missed: an uncovered `push` right after a function-ending instruction.
def ends_function(w):
    return ((w & 0x0FFF8000) == 0x08BD8000 or (w & 0x0FFFFFFF) in (0x012FFF1E, 0x01A0F00E, 0x049DF004)
            or (w & 0x0F000000) == 0x0A000000) and (w >> 28) == 0xE
n_pro = 0
for off in range(4, T1 - T0, 4):
    if cov[off // 4]:
        continue
    w = struct.unpack_from('<I', d, off)[0]
    prev = struct.unpack_from('<I', d, off - 4)[0]
    a = T0 + off
    if (w & 0xFFFF0000) == 0xE92D0000 and ends_function(prev) and a not in known and a not in cand and R.is_clean_code(a, known):
        cand.add(a); n_pro += 1
print(f'{n_pro} uncovered function prologues after a function end')
with open(out, 'a') as fh:
    for v in sorted(cand):
        fh.write(f'{v:08x} # stored pointer\n')
print(f'{len(cand)} new pointer-referenced entries appended to {out}')
