#!/usr/bin/env python3
"""armrecomp: static recompiler, ARM11 (ARMv6K + VFPv2, ARM mode) machine code -> C.

Each guest function becomes `void fn_XXXXXXXX(CPU *c)`. Guest memory keeps its original
addresses, so loads and stores are plain host memory accesses.

Usage: armrecomp.py <code_decompressed.bin> <functions.txt> <outdir> [--files N] [--jobs N]

The memory layout comes from exheader.bin next to the image. Idioms the control-flow analysis cannot
follow are found by pattern detectors (idioms.py) and written to <outdir>/specials.json. Hand-written
per-game settings live in games/<title id>.toml (see load_game_file).
"""
import collections, json, os, re, struct, sys
from multiprocessing import Pool
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import idioms, layout as layout_mod

# Set by configure(); never typed per game.
BASE = TEXT_END = RO_END = None     # text start, end of code actually present, end of read-only memory

# Functions that do not return to the instruction after the call (found by idioms.detect_inline_data_calls):
# target address -> resume offsets relative to the `bl`.
NONLOCAL_RETURN = {}

COND = ['c->z', '!c->z', 'c->cf', '!c->cf', 'c->n', '!c->n', 'c->v', '!c->v',
        '(c->cf && !c->z)', '(!c->cf || c->z)', '(c->n == c->v)', '(c->n != c->v)',
        '(!c->z && c->n == c->v)', '(c->z || c->n != c->v)']

IMG = b''


def configure(img, lay, nonlocal_returns):
    """Install the image, layout and detected idioms. Called in every worker process."""
    global IMG, BASE, TEXT_END, RO_END, NONLOCAL_RETURN
    IMG, BASE, TEXT_END, RO_END = img, lay.text_base, lay.text_end, lay.ro_end
    NONLOCAL_RETURN = nonlocal_returns


def rd32(a):
    return struct.unpack_from('<I', IMG, a - BASE)[0]


def in_text(a):
    return BASE <= a < TEXT_END and not (a & 3)


def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def rr(n, addr):
    """C expression reading guest register n at instruction addr."""
    return f'0x{(addr + 8) & 0xFFFFFFFF:08x}u' if n == 15 else f'c->r[{n}]'


class Unsup(Exception):
    pass


class Ins:
    __slots__ = ('addr', 'w', 'cond', 'body', 'falls', 'targets', 'calls', 'unsup', 'ptr', 'resume')

    def __init__(self, addr, w):
        self.addr, self.w, self.cond = addr, w, w >> 28
        self.body = None
        self.falls = True
        self.targets = []
        self.calls = []
        self.unsup = None
        self.ptr = None                 # code address taken through a literal pool
        self.resume = []                # local addresses a callee may come back to instead of addr+4


def G(t):
    return f'@G:{t:08x}@'


# ---------------------------------------------------------------- operand helpers
def shifter(w, addr):
    """Returns (pre_lines, value_expr, carry_out_expr) for a data-processing operand 2."""
    if w & (1 << 25):
        imm, rot = w & 0xFF, ((w >> 8) & 0xF) * 2
        v = ror32(imm, rot)
        return [], f'0x{v:x}u', ('c->cf' if rot == 0 else str(v >> 31))
    rm, st = w & 0xF, (w >> 5) & 3
    x = rr(rm, addr)
    if w & 0x10:
        if w & 0x80:
            raise Unsup('dp_bad_shift')
        rs = (w >> 8) & 0xF
        k = ('lsl', 'lsr', 'asr', 'ror')[st]
        return [f'uint32_t sco0 = c->cf; uint32_t b0 = sh_{k}_c({x}, c->r[{rs}] & 0xff, &sco0);'], 'b0', 'sco0'
    n = (w >> 7) & 0x1F
    if st == 0:
        return ([], x, 'c->cf') if n == 0 else ([], f'({x} << {n})', f'(({x} >> {32 - n}) & 1)')
    if st == 1:
        return ([], '0u', f'({x} >> 31)') if n == 0 else ([], f'({x} >> {n})', f'(({x} >> {n - 1}) & 1)')
    if st == 2:
        if n == 0:
            return [], f'(uint32_t)((int32_t){x} >> 31)', f'({x} >> 31)'
        return [], f'(uint32_t)((int32_t){x} >> {n})', f'(({x} >> {n - 1}) & 1)'
    if n == 0:
        return [], f'((c->cf << 31) | ({x} >> 1))', f'({x} & 1)'
    return [], f'(({x} >> {n}) | ({x} << {32 - n}))', f'(({x} >> {n - 1}) & 1)'


def ijump(ins, expr, is_return=False):
    """Jump to a computed address. `mov lr, pc` just before makes it a call; the usual
    return idioms (bx lr, pop {..., pc}, mov pc, lr) are compiled as plain returns."""
    prev = rd32(ins.addr - 4) if in_text(ins.addr - 4) else 0
    if (prev & 0x0FFFFFFF) == 0x01A0E00F and (prev >> 28) == ins.cond:
        ins.falls = True
        return f'call_indirect(c, {expr}); RET_CHECK(0x{ins.addr + 4:08x}u);'
    ins.falls = False
    site = f'SITE(0x{ins.addr:08x}u); '
    return site + (f'RET({expr});' if is_return else f'IJUMP({expr});')


def jt_bound(ins, rm):
    """Entry count of a jump table guarded by an earlier `cmp rm, #imm` and an LS/CC condition, or None."""
    if ins.cond not in (9, 3):
        return None
    a = ins.addr - 4
    for _ in range(256):                                  # compilers may schedule long runs of stores in between
        if not in_text(a):
            return None
        p = rd32(a)
        if (p & 0x0FF0F000) == 0x03500000 and ((p >> 16) & 0xF) == rm and (p >> 28) == 0xE:
            v = ror32(p & 0xFF, ((p >> 8) & 0xF) * 2)
            n = v + 1 if ins.cond == 9 else v
            return n if 0 < n <= 2048 else None
        cls = (p >> 25) & 7
        if cls == 5 or (p >> 28) != 0xE:                  # branch or conditional code in between
            return None
        if cls in (2, 3) and (p & (1 << 20)) and ((p >> 12) & 0xF) == rm:   # ldr into the index register
            return None
        if cls == 4 and (p & (1 << 20)) and (p >> rm) & 1:                  # ldm that includes it
            return None
        if cls in (2, 3, 4) and (p & (1 << 21)) and ((p >> 16) & 0xF) == rm:  # base writeback to it
            return None
        if cls == 6 and (p & (1 << 21)) and ((p >> 16) & 0xF) == rm:    # vldm / vstm writeback to it
            return None
        if cls == 7 and ((p >> 24) & 1 or ((p & 0x10) and (p & (1 << 20)) and ((p >> 12) & 0xF) in (rm, 15))):
            return None                                   # svc, or vmov / vmrs into it or into the flags
        if cls in (0, 1):
            is_load = (p & 0x0E000090) == 0x00000090 and (p >> 5) & 3
            if (p & (1 << 20)) and not is_load:           # sets flags
                return None
            if ((p >> 12) & 0xF) == rm and not is_load:   # index register rewritten
                return None
            if is_load and (p & (1 << 20)) and ((p >> 12) & 0xF) == rm:   # ldrh / ldrsb / ldrsh into it
                return None
            if (p & 0x0F0000F0) == 0x00000090 and ((p >> 16) & 0xF) == rm:  # mul / mla into it
                return None
        a -= 4
    return None


def jt_run_of_branches(addr):
    """Fallback for `add pc, pc, rm, lsl #2` with no visible bound: the table is a run of `b` instructions."""
    n = 0
    while in_text(addr + 8 + 4 * n) and (rd32(addr + 8 + 4 * n) >> 24) == 0xEA and n < 2048:
        n += 1
    return n + 1 if n else None                           # the last case may be the code right after the run


def selfrel_table(ins):
    """`add rT, pc, #imm` ; `ldr rX, [rT, rI, lsl #2]` ; `add pc, rX, rT` (or rT, rX): jump to table + table[rI].
    Returns (index register, targets) or None. The table length is not encoded, so entries are read
    while they point at nearby, cleanly decodable code."""
    w = ins.w
    ra, rb = (w >> 16) & 0xF, w & 0xF
    ld, ld_back = 0, 0
    for back in range(1, 4):                            # the table load, possibly a few instructions earlier
        p = rd32(ins.addr - 4 * back) if in_text(ins.addr - 4 * back) else 0
        if (p & 0x0FF00FF0) == 0x07900100 and (p >> 28) == 0xE and {(p >> 12) & 0xF, (p >> 16) & 0xF} == {ra, rb}:
            ld, ld_back = p, back
            break
        if ((p >> 12) & 0xF) in (ra, rb) or (p >> 25) & 7 in (4, 5):   # register rewritten, or ldm/branch
            return None
    if not ld:
        return None
    rx, rt, ri = (ld >> 12) & 0xF, (ld >> 16) & 0xF, ld & 0xF
    table = None
    for back in range(ld_back + 1, ld_back + 7):
        a = ins.addr - 4 * back
        if not in_text(a):
            break
        p = rd32(a)
        if (p & 0x0FBFF000) == (0x028F0000 | (rt << 12)) or (p & 0x0FFFF000) == (0x024F0000 | (rt << 12)):
            if (p >> 28) == 0xE:
                imm = ror32(p & 0xFF, ((p >> 8) & 0xF) * 2)
                table = (a + 8 + imm if (p & 0x00F00000) == 0x00800000 else a + 8 - imm) & 0xFFFFFFFF
                break
        if ((p >> 12) & 0xF) == rt:                      # table register overwritten by something else
            return None
    if table is None or not (BASE <= table < RO_END - 3):
        return None
    targets = []
    for k in range(256):
        if table + 4 * k + 4 > RO_END:
            break
        v = rd32(table + 4 * k)
        t = (table + v) & 0xFFFFFFFF
        if v >= 0x8000 or not in_text(t) or decode(t).unsup:
            break
        targets.append(t)
    return (ri, targets) if len(targets) >= 2 else None


def emit_switch(ins, rm, targets):
    for t in targets:
        if not in_text(t):
            raise Unsup('jumptable_bad_target')
    ins.targets = list(dict.fromkeys(targets))
    ins.falls = False
    cases = ' '.join(f'case {i}: {G(t)}' for i, t in enumerate(targets))
    return f'switch (c->r[{rm}]) {{ {cases} }} TRAP(0x{ins.addr:08x}u, "jump table index out of range");'


# ---------------------------------------------------------------- integer instructions
def dp(ins):
    w, addr = ins.w, ins.addr
    opc, S, rn, rd = (w >> 21) & 0xF, (w >> 20) & 1, (w >> 16) & 0xF, (w >> 12) & 0xF
    if (w & 0x0FF0FFF0) == 0x0080F000 and ins.cond == 14:   # add pc, rA, rB: self-relative jump table
        tgt = selfrel_table(ins)
        if tgt:
            # dispatch on the computed address: the load may have reused the index register
            targets = tgt[1]
            ins.targets = list(dict.fromkeys(targets))
            ins.falls = False
            cases = ' '.join(f'case 0x{t:08x}u: {G(t)}' for t in ins.targets)
            return (f'uint32_t t = c->r[{(w >> 16) & 0xF}] + c->r[{w & 0xF}]; switch (t) {{ {cases} }} '
                    f'IJUMP(t);')
    if (w & 0x0FFFFFF0) == 0x008FF100:                    # add pc, pc, rm, lsl #2
        n = jt_bound(ins, w & 0xF) or jt_run_of_branches(addr)
        if n is None:
            raise Unsup('jumptable_unbounded')
        return emit_switch(ins, w & 0xF, [addr + 8 + 4 * i for i in range(n)])
    logical = opc in (0, 1, 8, 9, 12, 13, 14, 15)
    test = opc in (8, 9, 10, 11)
    if (w & 0x0FFF0FF0) == 0x008F0000 and rd != 15:        # add rd, pc, rm: position-independent address
        a2 = addr - 4
        for _ in range(8):
            if not in_text(a2):
                break
            p = rd32(a2)
            if (p & 0xFF7FF000) == (0xE51F0000 | ((w & 0xF) << 12)):
                lit = (a2 + 8 + (p & 0xFFF if p & (1 << 23) else -(p & 0xFFF))) & 0xFFFFFFFF
                if BASE <= lit < RO_END - 3:
                    t = (addr + 8 + rd32(lit)) & 0xFFFFFFFF
                    if in_text(t):
                        ins.ptr = t
                break
            a2 -= 4
    if (w & 0x0FFF0000) in (0x028F0000, 0x024F0000) and rd < 14:   # adr rd, label: may be a function pointer
        imm = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
        t = (addr + 8 + (imm if opc == 4 else -imm)) & 0xFFFFFFFF
        if in_text(t):
            ins.ptr = t
    if (w & 0x0FFFF000) in (0x028EE000, 0x024EE000):       # add/sub lr, lr, #imm after add/sub lr, pc, #imm
        for back in (1, 2, 3):
            pa = addr - 4 * back
            if not in_text(pa):
                break
            p = rd32(pa)
            if (p & 0x0FFFF000) in (0x028FE000, 0x024FE000) and (p >> 28) == 0xE:
                i1 = ror32(p & 0xFF, ((p >> 8) & 0xF) * 2)
                base_v = pa + 8 + i1 if (p & 0x00F00000) == 0x00800000 else pa + 8 - i1
                i2 = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
                t = (base_v + i2 if opc == 4 else base_v - i2) & 0xFFFFFFFF
                if in_text(t):
                    ins.resume = [t]
                    ins.targets = [t]
                break
            if ((p >> 12) & 0xF) == 14:                    # lr written by something else in between
                break
    if (w & 0x0FFFF000) in (0x028FE000, 0x024FE000):       # add/sub lr, pc, #imm: hand-made return address
        imm = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
        t = (addr + 8 + (imm if opc == 4 else -imm)) & 0xFFFFFFFF
        if in_text(t):
            ins.resume = [t]
            ins.targets = [t]
    pre, op2, sco = shifter(w, addr)
    L = list(pre) + [f'uint32_t b = {op2};']
    if S and logical:
        L.append(f'uint32_t sco = {sco};')
    if rd == 15 and S and not test:
        raise Unsup('dp_s_pc')
    a = rr(rn, addr)
    arith = {2: (a, '~b', '1'), 3: ('b', f'~{a}', '1'), 4: (a, 'b', '0'), 5: (a, 'b', 'c->cf'),
             6: (a, '~b', 'c->cf'), 7: ('b', f'~{a}', 'c->cf'), 10: (a, '~b', '1'), 11: (a, 'b', '0')}
    if opc in arith:
        x, y, cin = arith[opc]
        res = f'add_f(c, {x}, {y}, {cin})' if S else f'{x} + {y} + {cin}'
    else:
        res = {0: f'{a} & b', 1: f'{a} ^ b', 8: f'{a} & b', 9: f'{a} ^ b', 12: f'{a} | b',
               13: 'b', 14: f'{a} & ~b', 15: '~b'}[opc]
    L.append(f'uint32_t res = {res};')
    if S and logical:
        L.append('c->n = res >> 31; c->z = (res == 0); c->cf = sco;')
    if not test:
        is_ret = (w & 0x0FFFFFFF) == 0x01A0F00E            # mov pc, lr
        L.append(ijump(ins, 'res', is_ret) if rd == 15 else f'c->r[{rd}] = res;')
    return ' '.join(L)


def ea_lines(addr, rn, off, P, U, W):
    """Address computation. Returns (lines, writeback_stmt)."""
    base, sign = rr(rn, addr), '+' if U else '-'
    if P:
        return [f'uint32_t ea = {base} {sign} {off};'], (f'c->r[{rn}] = ea;' if W else '')
    return [f'uint32_t off = {off}; uint32_t ea = {base};'], f'c->r[{rn}] = ea {sign} off;'


def ldr_str(ins):
    w, addr = ins.w, ins.addr
    P, U, B, W, Ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
    if (w & 0x0FFFFFF0) == 0x079FF100:                    # ldr pc, [pc, rm, lsl #2]
        n = jt_bound(ins, w & 0xF)
        if n is None:
            raise Unsup('jumptable_unbounded')
        return emit_switch(ins, w & 0xF, [rd32(addr + 8 + 4 * i) for i in range(n)])
    if w & (1 << 25):
        if w & 0x10:
            raise Unsup('media')
        _, off, _ = shifter(w & ~(1 << 25) & 0xFFFFFFFF, addr)
    else:
        off = f'0x{w & 0xFFF:x}u'
    if rn == 15 and (W or not P):
        raise Unsup('ldr_pc_writeback')
    # literal pool load from read-only memory: fold to a constant
    if rn == 15 and not (w & (1 << 25)) and Ld and not B and rd != 15:
        ea = (addr + 8 + (w & 0xFFF if U else -(w & 0xFFF))) & 0xFFFFFFFF
        if BASE <= ea < RO_END - 3:
            v = rd32(ea)
            if in_text(v):
                ins.ptr = v
            return f'c->r[{rd}] = 0x{v:08x}u;'
    L, wb = ea_lines(addr, rn, off, P, U, W)
    if Ld:
        L.append(wb)
        if B:
            if rd == 15:
                raise Unsup('ldrb_pc')
            L.append(f'c->r[{rd}] = ld8(ea);')
        elif rd == 15:
            L.append('uint32_t t = ld32(ea);')
            L.append(ijump(ins, 't', rn == 13))             # ldr pc, [sp], #4 is a return
        else:
            L.append(f'c->r[{rd}] = ld32(ea);')
    else:
        val = f'0x{(addr + 12) & 0xFFFFFFFF:08x}u' if rd == 15 else f'c->r[{rd}]'
        L.append(f'uint32_t val = {val};')
        L.append(wb)
        L.append(f'st8(ea, val);' if B else 'st32(ea, val);')
    return ' '.join(x for x in L if x)


def ldr_str_h(ins):
    w, addr = ins.w, ins.addr
    P, U, I, W, Ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    rn, rd, sh = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 5) & 3
    off = f'0x{((w >> 4) & 0xF0) | (w & 0xF):x}u' if I else rr(w & 0xF, addr)
    if rd == 15 or (rn == 15 and (W or not P)):
        raise Unsup('ldrh_pc')
    L, wb = ea_lines(addr, rn, off, P, U, W)
    if Ld:
        L.append(wb)
        L.append({1: f'c->r[{rd}] = ld16(ea);',
                  2: f'c->r[{rd}] = (uint32_t)(int32_t)(int8_t)ld8(ea);',
                  3: f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)ld16(ea);'}[sh])
    elif sh == 1:
        L.append(f'uint32_t val = c->r[{rd}];')
        L.append(wb)
        L.append('st16(ea, val);')
    else:
        if rd & 1 or rd == 14:
            raise Unsup('ldrd_odd')
        if sh == 2:                                         # LDRD
            L.append(wb)
            L.append(f'uint32_t lo = ld32(ea), hi = ld32(ea + 4); c->r[{rd}] = lo; c->r[{rd + 1}] = hi;')
        else:                                               # STRD
            L.append(f'uint32_t lo = c->r[{rd}], hi = c->r[{rd + 1}];')
            L.append(wb)
            L.append('st32(ea, lo); st32(ea + 4, hi);')
    return ' '.join(x for x in L if x)


def ldm_stm(ins):
    w, addr = ins.w, ins.addr
    P, U, S, W, Ld, rn = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1, (w >> 16) & 0xF
    regs = [i for i in range(16) if (w >> i) & 1]
    n = len(regs)
    if S or n == 0 or rn == 15:
        raise Unsup('ldm_special')
    if U:
        start = 'bs + 4' if P else 'bs'
    else:
        start = f'bs - {4 * n}' if P else f'bs - {4 * n - 4}'
    wbv = f'bs + {4 * n}' if U else f'bs - {4 * n}'
    L = [f'uint32_t bs = c->r[{rn}]; uint32_t a = {start};']
    if Ld:
        if W and rn not in regs:
            L.append(f'c->r[{rn}] = {wbv};')
        jump = None
        for k, r in enumerate(regs):
            if r == 15:
                L.append(f'uint32_t t = ld32(a + {4 * k});')
                jump = ijump(ins, 't', rn == 13)            # pop {..., pc}
            else:
                L.append(f'c->r[{r}] = ld32(a + {4 * k});')
        if jump:
            L.append(jump)
    else:
        for k, r in enumerate(regs):
            v = f'0x{(addr + 12) & 0xFFFFFFFF:08x}u' if r == 15 else ('bs' if r == rn else f'c->r[{r}]')
            L.append(f'st32(a + {4 * k}, {v});')
        if W:
            L.append(f'c->r[{rn}] = {wbv};')
    return ' '.join(L)


def branch(ins):
    w, addr = ins.w, ins.addr
    off = w & 0xFFFFFF
    if off & 0x800000:
        off -= 0x1000000
    t = (addr + 8 + off * 4) & 0xFFFFFFFF
    if not in_text(t):
        raise Unsup('branch_out_of_text')
    if w & (1 << 24):
        ins.calls = [t]
        if t in NONLOCAL_RETURN:
            ins.resume = [addr + o for o in NONLOCAL_RETURN[t] if in_text(addr + o)]
            ins.targets = list(ins.resume)
            ins.falls = False
            return f'c->r[14] = 0x{addr + 4:08x}u; @C:{t:08x}@ goto L_resume;'
        return f'c->r[14] = 0x{addr + 4:08x}u; @C:{t:08x}@ RET_CHECK(0x{addr + 4:08x}u);'
    ins.targets = [t]
    ins.falls = False
    return G(t)


def half(reg, hi):
    return f'(int32_t)(int16_t)({reg} >> 16)' if hi else f'(int32_t)(int16_t){reg}'


def misc0(ins):
    """Class 000 instructions that are not plain data processing. Returns None if not matched."""
    w, addr = ins.w, ins.addr
    f = lambda sh: (w >> sh) & 0xF
    if (w & 0x0FFFFFF0) == 0x012FFF10:                    # BX
        return ijump(ins, rr(w & 0xF, addr), (w & 0xF) == 14)
    if (w & 0x0FFFFFF0) == 0x012FFF30:                    # BLX reg
        return f'uint32_t t = {rr(w & 0xF, addr)}; c->r[14] = 0x{addr + 4:08x}u; call_indirect(c, t); RET_CHECK(0x{addr + 4:08x}u);'
    if (w & 0x0FFF0FF0) == 0x016F0F10:                    # CLZ
        return f'c->r[{f(12)}] = clz32(c->r[{f(0)}]);'
    if (w & 0x0FC000F0) == 0x00000090:                    # MUL / MLA
        rd, rn, rs, rm = f(16), f(12), f(8), f(0)
        acc = f' + c->r[{rn}]' if w & (1 << 21) else ''
        s = ' c->n = res >> 31; c->z = (res == 0);' if w & (1 << 20) else ''
        return f'uint32_t res = c->r[{rm}] * c->r[{rs}]{acc}; c->r[{rd}] = res;{s}'
    if (w & 0x0FF000F0) == 0x00400090:                    # UMAAL
        hi, lo, rs, rm = f(16), f(12), f(8), f(0)
        return (f'uint64_t res = (uint64_t)c->r[{rm}] * c->r[{rs}] + c->r[{lo}] + c->r[{hi}]; '
                f'c->r[{lo}] = (uint32_t)res; c->r[{hi}] = (uint32_t)(res >> 32);')
    if (w & 0x0F8000F0) == 0x00800090:                    # UMULL UMLAL SMULL SMLAL
        hi, lo, rs, rm = f(16), f(12), f(8), f(0)
        if w & (1 << 22):
            m = f'(uint64_t)((int64_t)(int32_t)c->r[{rm}] * (int32_t)c->r[{rs}])'
        else:
            m = f'(uint64_t)c->r[{rm}] * c->r[{rs}]'
        acc = f' + (((uint64_t)c->r[{hi}] << 32) | c->r[{lo}])' if w & (1 << 21) else ''
        s = ' c->n = (uint32_t)(res >> 63); c->z = (res == 0);' if w & (1 << 20) else ''
        return f'uint64_t res = {m}{acc}; c->r[{lo}] = (uint32_t)res; c->r[{hi}] = (uint32_t)(res >> 32);{s}'
    if (w & 0x0FB00FF0) == 0x01000090:                    # SWP / SWPB
        rn, rd, rm = f(16), f(12), f(0)
        if w & (1 << 22):
            return f'uint32_t a = c->r[{rn}]; uint32_t t = ld8(a); st8(a, c->r[{rm}]); c->r[{rd}] = t;'
        return f'uint32_t a = c->r[{rn}]; uint32_t t = ld32(a); st32(a, c->r[{rm}]); c->r[{rd}] = t;'
    if (w & 0x0F900FFF) == 0x01900F9F:                    # LDREX / LDREXD / LDREXB / LDREXH
        rn, rd, k = f(16), f(12), (w >> 21) & 3
        if k == 1:
            return f'uint32_t a = c->r[{rn}]; uint32_t lo = ld32(a), hi = ld32(a + 4); c->r[{rd}] = lo; c->r[{rd + 1}] = hi;'
        return f'c->r[{rd}] = {("ld32", "", "ld8", "ld16")[k]}(c->r[{rn}]);'
    if (w & 0x0F900FF0) == 0x01800F90:                    # STREX family (always succeeds: guest threads are cooperative)
        rn, rd, rm, k = f(16), f(12), f(0), (w >> 21) & 3
        if k == 1:
            return f'uint32_t a = c->r[{rn}]; st32(a, c->r[{rm}]); st32(a + 4, c->r[{rm + 1}]); c->r[{rd}] = 0;'
        return f'{("st32", "", "st8", "st16")[k]}(c->r[{rn}], c->r[{rm}]); c->r[{rd}] = 0;'
    if (w & 0x0E000090) == 0x00000090:
        if (w >> 5) & 3:
            return ldr_str_h(ins)
        raise Unsup('undefined_000_1001')
    if (w & 0x0FBF0FFF) == 0x010F0000:                    # MRS
        if w & (1 << 22):
            raise Unsup('mrs_spsr')
        return f'c->r[{f(12)}] = get_cpsr(c);'
    if (w & 0x0FB0FFF0) == 0x0120F000:                    # MSR reg
        if w & (1 << 22):
            raise Unsup('msr_spsr')
        return f'set_cpsr_f(c, c->r[{f(0)}]);' if w & (1 << 19) else ';'
    if (w & 0x0F900FF0) == 0x01000050:                    # QADD QSUB QDADD QDSUB
        rn, rd, rm, k = f(16), f(12), f(0), (w >> 21) & 3
        a, b = f'(int64_t)(int32_t)c->r[{rm}]', f'(int64_t)(int32_t)c->r[{rn}]'
        if k & 2:
            b = f'(int64_t)(int32_t)sat_s64(c, 2 * {b})'
        return f'c->r[{rd}] = sat_s64(c, {a} {"-" if k & 1 else "+"} {b});'
    if (w & 0x0F900090) == 0x01000080:                    # SMLAxy SMLAWy SMULWy SMLALxy SMULxy
        rd, rn, rs, rm, k = f(16), f(12), f(8), f(0), (w >> 21) & 3
        x, y = (w >> 5) & 1, (w >> 6) & 1
        hm, hs = half(f'c->r[{rm}]', x), half(f'c->r[{rs}]', y)
        if k == 3:
            return f'c->r[{rd}] = (uint32_t)({hm} * {hs});'
        if k == 0:
            return (f'int32_t p = {hm} * {hs}; int64_t t = (int64_t)p + (int32_t)c->r[{rn}]; '
                    f'if (t != (int32_t)t) c->q = 1; c->r[{rd}] = (uint32_t)t;')
        if k == 1:
            p = f'(int32_t)(((int64_t)(int32_t)c->r[{rm}] * {hs}) >> 16)'
            if x:
                return f'c->r[{rd}] = (uint32_t){p};'
            return f'int64_t t = (int64_t){p} + (int32_t)c->r[{rn}]; if (t != (int32_t)t) c->q = 1; c->r[{rd}] = (uint32_t)t;'
        return (f'uint64_t res = (uint64_t)(int64_t)({hm} * {hs}) + (((uint64_t)c->r[{rd}] << 32) | c->r[{rn}]); '
                f'c->r[{rn}] = (uint32_t)res; c->r[{rd}] = (uint32_t)(res >> 32);')
    if (w & 0x01900000) == 0x01000000:
        raise Unsup('misc_000')
    return None


def media(ins):
    w, addr = ins.w, ins.addr
    f = lambda sh: (w >> sh) & 0xF
    rd, rm = f(12), f(0)
    if (w & 0x0F8000F0) == 0x06800070:                    # sign/zero extend (+ add)
        op, rn, rot = (w >> 20) & 7, f(16), ((w >> 10) & 3) * 8
        x = f'c->r[{rm}]' if rot == 0 else f'((c->r[{rm}] >> {rot}) | (c->r[{rm}] << {32 - rot}))'
        e = {2: f'(uint32_t)(int32_t)(int8_t){x}', 3: f'(uint32_t)(int32_t)(int16_t){x}',
             6: f'({x} & 0xffu)', 7: f'({x} & 0xffffu)'}.get(op)
        if op in (0, 4):                                   # SXTAB16/SXTB16, UXTAB16/UXTB16
            lo = f'(uint32_t)(int32_t)(int8_t){x}' if op == 0 else f'({x} & 0xffu)'
            hi = f'(uint32_t)(int32_t)(int8_t)({x} >> 16)' if op == 0 else f'(({x} >> 16) & 0xffu)'
            if rn != 15:
                lo, hi = f'({lo} + c->r[{rn}])', f'({hi} + (c->r[{rn}] >> 16))'
            return f'uint32_t lo = {lo}, hi = {hi}; c->r[{rd}] = (lo & 0xffffu) | (hi << 16);'
        if e is None:
            raise Unsup('xt16')
        return f'c->r[{rd}] = {e}{"" if rn == 15 else f" + c->r[{rn}]"};'
    if (w & 0x0FFF0FF0) == 0x06BF0F30:
        return f'c->r[{rd}] = __builtin_bswap32(c->r[{rm}]);'
    if (w & 0x0FFF0FF0) == 0x06BF0FB0:
        return f'uint32_t x = c->r[{rm}]; c->r[{rd}] = ((x >> 8) & 0x00ff00ffu) | ((x << 8) & 0xff00ff00u);'
    if (w & 0x0FFF0FF0) == 0x06FF0FB0:
        return f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)c->r[{rm}]);'
    if (w & 0x0FA00030) == 0x06A00010:                    # SSAT / USAT
        sat, n, asr = (w >> 16) & 0x1F, (w >> 7) & 0x1F, (w >> 6) & 1
        if asr:
            x = f'((int32_t)c->r[{rm}] >> {n if n else 31})'
        else:
            x = f'(int32_t)(c->r[{rm}] << {n})'
        if w & (1 << 22):
            return f'c->r[{rd}] = usat_n(c, {x}, {sat});'
        return f'c->r[{rd}] = ssat_n(c, {x}, {sat + 1});'
    if (w & 0x0FF00030) == 0x06800010:                    # PKHBT / PKHTB
        rn, n = f(16), (w >> 7) & 0x1F
        if w & 0x40:
            return f'c->r[{rd}] = (c->r[{rn}] & 0xffff0000u) | ((uint32_t)((int32_t)c->r[{rm}] >> {n if n else 31}) & 0xffffu);'
        return f'c->r[{rd}] = (c->r[{rn}] & 0xffffu) | ((c->r[{rm}] << {n}) & 0xffff0000u);'
    if (w & 0x0FB00FF0) == 0x06100FF0:                    # SSUB8 / USUB8, set GE
        return f'c->r[{rd}] = sub8_ge(c, c->r[{f(16)}], c->r[{rm}], {0 if w & (1 << 22) else 1});'
    if (w & 0x0FF00FF0) == 0x06800FB0:                    # SEL
        return f'c->r[{rd}] = sel8(c, c->r[{f(16)}], c->r[{rm}]);'
    if (w & 0x0FF00FF0) == 0x06600F90:                    # UQADD8
        return f'c->r[{rd}] = uqadd8(c->r[{f(16)}], c->r[{rm}]);'
    if (w & 0x0FF00FF0) == 0x06500F90:                    # UADD8, sets GE
        return f'c->r[{rd}] = uadd8_ge(c, c->r[{f(16)}], c->r[{rm}]);'
    if (w & 0x0FF00FF0) == 0x06600FF0:                    # UQSUB8
        return f'c->r[{rd}] = uqsub8(c->r[{f(16)}], c->r[{rm}]);'
    if (w & 0x0FF00FF0) == 0x06700F90:                    # UHADD8
        return (f'uint32_t x = c->r[{f(16)}], y = c->r[{rm}]; '
                f'c->r[{rd}] = (x & y) + (((x ^ y) & 0xfefefefeu) >> 1);')
    if (w & 0x0FF00090) == 0x07500010:                    # SMMUL SMMLA SMMLS
        rd2, ra, rm2, rn2, rnd = f(16), f(12), f(8), f(0), (w >> 5) & 1
        p = f'(int64_t)(int32_t)c->r[{rn2}] * (int32_t)c->r[{rm2}]'
        r = ' + 0x80000000ll' if rnd else ''
        if (w >> 6) & 3 == 3:
            return f'c->r[{rd2}] = (uint32_t)((((int64_t)(int32_t)c->r[{ra}] << 32) - {p}{r}) >> 32);'
        if (w >> 6) & 3 == 0:
            acc = '' if ra == 15 else f'((int64_t)(int32_t)c->r[{ra}] << 32) + '
            return f'c->r[{rd2}] = (uint32_t)(({acc}{p}{r}) >> 32);'
    raise Unsup('media')


# ---------------------------------------------------------------- VFP
def vfp_cdp(ins):
    w = ins.w
    dbl = ((w >> 8) & 0xF) == 11
    p, D, q, r = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    N, s, M = (w >> 7) & 1, (w >> 6) & 1, (w >> 5) & 1
    fn4, fd4, fm4 = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
    sd_, sn_, sm_ = (fd4 << 1) | D, (fn4 << 1) | N, (fm4 << 1) | M     # single-precision indexes
    if dbl:
        Gt, St, t = 'GD', 'SD', 'd'
        d, n, m = fd4, fn4, fm4
    else:
        Gt, St, t = 'GS', 'SS', 'f'
        d, n, m = sd_, sn_, sm_
    op = (p << 3) | (q << 2) | (r << 1) | s
    if op != 0xF:
        if dbl and (D or N or M):
            raise Unsup('vfp_d16_31')
        gd, gn, gm = f'{Gt}({d})', f'{Gt}({n})', f'{Gt}({m})'
        mul = f'vmul{t}({gn}, {gm})'
        e = {0x0: f'vadd{t}({gd}, {mul})',
             0x1: f'vadd{t}({gd}, vneg{t}({mul}))',
             0x2: f'vadd{t}(vneg{t}({gd}), {mul})',
             0x3: f'vadd{t}(vneg{t}({gd}), vneg{t}({mul}))',
             0x4: mul,
             0x5: f'vneg{t}({mul})',
             0x6: f'vadd{t}({gn}, {gm})',
             0x7: f'vsub{t}({gn}, {gm})',
             0x8: f'vdiv{t}({gn}, {gm})'}.get(op)
        if e is None:
            raise Unsup('vfp_cdp')
        return f'{St}({d}, {e});'
    # extension space: fn4 selects the operation, N is bit 7
    ext = (fn4 << 1) | N
    if dbl and (M or (D and ext not in (0x0F, 0x18, 0x19, 0x1A, 0x1B))):
        if ext not in (0x10, 0x11):                        # int->double takes a single-precision source
            raise Unsup('vfp_d16_31')
    gm = f'{Gt}({m})'
    if ext == 0x00:
        return f'c->s[{2 * d}] = c->s[{2 * m}]; c->s[{2 * d + 1}] = c->s[{2 * m + 1}];' if dbl else f'c->s[{d}] = c->s[{m}];'
    if ext == 0x01:
        return f'{St}({d}, vabs{t}({gm}));'
    if ext == 0x02:
        return f'{St}({d}, vneg{t}({gm}));'
    if ext == 0x03:
        return f'{St}({d}, vsqrt{t}({gm}));'
    if ext in (0x08, 0x09):
        return f'VCMP{t.upper()}({Gt}({d}), {gm});'
    if ext in (0x0A, 0x0B):
        return f'VCMP{t.upper()}({Gt}({d}), 0);'
    if ext == 0x0F:                                        # single <-> double
        return f'SS({sd_}, vcvt_d2f(GD({fm4})));' if dbl else f'SD({fd4}, vcvt_f2d(GS({sm_})));'
    if ext in (0x10, 0x11):                                # unsigned / signed int -> fp
        src = f'(int32_t)c->s[{sm_}]' if ext == 0x11 else f'c->s[{sm_}]'
        return f'SD({fd4}, (double){src});' if dbl else f'SS({sd_}, (float){src});'
    if ext in (0x18, 0x19, 0x1A, 0x1B):                    # fp -> unsigned (0x18/19) / signed (0x1A/1B) int
        src = f'GD({fm4})' if dbl else f'(double)GS({sm_})'
        if not (ext & 1):
            src = f'nearbyint({src})'                      # round using the current FPSCR mode
        return f'c->s[{sd_}] = vfp_d2{"s" if ext & 2 else "u"}i({src});'
    raise Unsup('vfp_ext')


def vfp_xfer(ins):
    w = ins.w
    dbl = ((w >> 8) & 0xF) == 11
    opc1, Ld, fn4, rd, N = (w >> 21) & 7, (w >> 20) & 1, (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 7) & 1
    if w & 0x6F:
        raise Unsup('vfp_xfer')
    if not dbl and opc1 == 7:                              # FMXR / FMRX
        if fn4 == 1:
            if not Ld:
                return f'vfp_set_fpscr(c, c->r[{rd}]);'
            return 'vfp_fmstat(c);' if rd == 15 else f'c->r[{rd}] = c->fpscr;'
        if Ld and rd != 15 and fn4 in (0, 8):
            return f'c->r[{rd}] = 0x{0x410120B4 if fn4 == 0 else 0x40000000:08x}u;'
        if not Ld and fn4 == 8:
            return ';'
        raise Unsup('vfp_sysreg')
    if rd == 15:
        raise Unsup('vfp_xfer_pc')
    if not dbl and opc1 == 0:
        i = (fn4 << 1) | N
    elif dbl and opc1 in (0, 1) and not N:
        i = 2 * fn4 + opc1
    else:
        raise Unsup('vfp_xfer')
    return f'c->r[{rd}] = c->s[{i}];' if Ld else f'c->s[{i}] = c->r[{rd}];'


def vfp_ls(ins):
    w, addr = ins.w, ins.addr
    dbl = ((w >> 8) & 0xF) == 11
    P, U, D, W, Ld = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
    rn, fd4, off8 = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xFF
    if (w & 0x0FE000D0) == 0x0C400010:                     # two-register transfer
        rn2, rd2, m4, M = rn, fd4, w & 0xF, (w >> 5) & 1
        if dbl:
            if M:
                raise Unsup('vfp_d16_31')
            lo, hi = 2 * m4, 2 * m4 + 1
        else:
            lo = (m4 << 1) | M
            hi = lo + 1
            if hi > 31:
                raise Unsup('vfp_xfer')
        if Ld:
            return f'uint32_t lo = c->s[{lo}], hi = c->s[{hi}]; c->r[{rd2}] = lo; c->r[{rn2}] = hi;'
        return f'c->s[{lo}] = c->r[{rd2}]; c->s[{hi}] = c->r[{rn2}];'
    if dbl and D:
        raise Unsup('vfp_d16_31')
    first = 2 * fd4 if dbl else (fd4 << 1) | D             # index into c->s[]
    if P and not W:                                        # FLDS/FLDD/FSTS/FSTD
        words = 2 if dbl else 1
        if rn == 15:
            ea_c = (addr + 8 + (off8 * 4 if U else -off8 * 4)) & 0xFFFFFFFF
            if Ld and BASE <= ea_c < RO_END - 7:
                return ' '.join(f'c->s[{first + k}] = 0x{rd32(ea_c + 4 * k):08x}u;' for k in range(words))
            ea = f'0x{ea_c:08x}u'
        else:
            ea = f'c->r[{rn}] {"+" if U else "-"} {off8 * 4}'
        L = [f'uint32_t ea = {ea};']
        for k in range(words):
            L.append(f'c->s[{first + k}] = ld32(ea + {4 * k});' if Ld else f'st32(ea + {4 * k}, c->s[{first + k}]);')
        return ' '.join(L)
    if (P, U, W) not in ((0, 1, 0), (0, 1, 1), (1, 0, 1)) or rn == 15:
        raise Unsup('vfp_ls')
    words = (off8 & ~1) if dbl else off8
    if words == 0 or first + words > 32:
        raise Unsup('vfp_ls')
    L = [f'uint32_t a = c->r[{rn}]{"" if U else f" - {off8 * 4}"};']
    if W:
        L.append(f'c->r[{rn}] = c->r[{rn}] {"+" if U else "-"} {off8 * 4};')
    for k in range(words):
        L.append(f'c->s[{first + k}] = ld32(a + {4 * k});' if Ld else f'st32(a + {4 * k}, c->s[{first + k}]);')
    return ' '.join(L)


# ---------------------------------------------------------------- top-level decode
def decode_body(ins):
    w, addr = ins.w, ins.addr
    if ins.cond == 0xF:
        if (w & 0xFD70F000) == 0xF550F000 or w == 0xF57FF01F:   # PLD, CLREX
            return ';'
        if (w & 0xFE000000) == 0xFA000000:
            raise Unsup('blx_thumb')
        raise Unsup('uncond')
    op = (w >> 25) & 7
    if op == 0:
        r = misc0(ins)
        return r if r is not None else dp(ins)
    if op == 1:
        if (w & 0x0FB0F000) == 0x0320F000:                # MSR imm
            if w & (1 << 22):
                raise Unsup('msr_spsr')
            v = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
            return f'set_cpsr_f(c, 0x{v:08x}u);' if w & (1 << 19) else ';'
        if (w & 0x01900000) == 0x01000000:
            raise Unsup('undefined_001')
        return dp(ins)
    if op == 2:
        return ldr_str(ins)
    if op == 3:
        return media(ins) if (w & 0x10) and (w & 0x0FFFFFF0) != 0x079FF100 else ldr_str(ins)
    if op == 4:
        return ldm_stm(ins)
    if op == 5:
        return branch(ins)
    cp = (w >> 8) & 0xF
    if op == 6:
        if cp in (10, 11):
            return vfp_ls(ins)
        raise Unsup('coproc')
    if w & (1 << 24):
        return f'svc_call(c, 0x{w & 0xFFFFFF:x}u, 0x{addr:08x}u);'
    if cp in (10, 11):
        return vfp_xfer(ins) if w & 0x10 else vfp_cdp(ins)
    if (w & 0x0FFF0FDF) == 0x0E070F9A:                    # mcr p15, 0, rd, c7, c10, 4/5 (memory barriers)
        return ';'
    if (w & 0x0FFF0FFF) == 0x0E1D0F70:                    # mrc p15, 0, rd, c13, c0, 3 (thread pointer)
        return f'c->r[{(w >> 12) & 0xF}] = c->tls;'
    raise Unsup('coproc')


def decode(addr):
    ins = Ins(addr, rd32(addr))
    try:
        ins.body = decode_body(ins)
    except Unsup as e:
        ins.unsup = e.args[0]
        ins.targets, ins.calls = [], []
        ins.falls = False
        ins.body = f'TRAP_UNIMPL(0x{addr:08x}u, 0x{ins.w:08x}u); /* {ins.unsup} */'
    if ins.cond < 14:
        ins.falls = True
    return ins


# ---------------------------------------------------------------- function discovery / emission
MAX_FN_INSNS = 60000


def discover(entry, funcs):
    """Recursive descent over direct branches. Returns {addr: Ins}."""
    body, work = {}, [entry]
    while work:
        a = work.pop()
        while a not in body and in_text(a) and (a == entry or a not in funcs) and len(body) < MAX_FN_INSNS:
            ins = decode(a)
            body[a] = ins
            for t in ins.targets:
                if (t == entry or t not in funcs) and t not in body:
                    work.append(t)
            if not ins.falls:
                break
            a += 4
    return body


def is_clean_code(t, funcs):
    """True when recursive descent from t only meets valid instructions (so t is code, not data)."""
    body = discover(t, funcs)
    return 0 < len(body) < MAX_FN_INSNS and all(i.unsup is None for i in body.values())


PLACEHOLDER = re.compile(r'@([GC]):([0-9a-f]{8})@')


def emit_function(entry, body, funcs):
    labels = set()
    for ins in body.values():
        labels.update(t for t in ins.targets if t in body)

    resume = sorted({t for ins in body.values() for t in ins.resume})
    leave = 'goto L_resume;' if resume else 'return;'

    def resolve(m):
        kind, t = m.group(1), int(m.group(2), 16)
        if kind == 'C':
            return f'fn_{t:08x}(c);'
        if t in body:
            return f'goto L_{t:08x};'
        if t in funcs:
            return f'{{ fn_{t:08x}(c); {leave} }}'
        return f'TRAP(0x{t:08x}u, "branch to unknown code");'

    order = sorted(body)
    out = [f'void fn_{entry:08x}(CPU *c) {{', '  uint32_t ret_lr = c->r[14]; (void)ret_lr;']
    if order[0] != entry:
        labels.add(entry)
        out.append(f'  goto L_{entry:08x};')
    for a in order:
        ins = body[a]
        if a in labels:
            out.append(f'L_{a:08x}:;')
        code = PLACEHOLDER.sub(resolve, ins.body)
        if resume:
            code = code.replace('IJUMP(', 'IJUMP_R(').replace('RET(', 'RET_R(')
        out.append(f'  if ({COND[ins.cond]}) {{ {code} }}' if ins.cond < 14 else f'  {{ {code} }}')
        if ins.falls and (a + 4) not in body:
            if (a + 4) in funcs:
                out.append(f'  fn_{a + 4:08x}(c); {leave}')
            else:
                out.append(f'  TRAP(0x{a + 4:08x}u, "fell off the end of a function");')
    if resume:
        # a callee came back with r15 set: continue at that local address, or keep returning
        cases = ' '.join(f'case 0x{t:08x}u: {PLACEHOLDER.sub(resolve, G(t))}' for t in resume)
        out.append(f'  return;\nL_resume:;\n  switch (c->r[15]) {{ {cases} default: return; }}')
    out.append('}')
    return '\n'.join(out)


# ---------------------------------------------------------------- driver
_FUNCS = None


def _init(img_path, funcs, lay, nonlocal_returns):
    global _FUNCS
    configure(open(img_path, 'rb').read(), lay, nonlocal_returns)
    _FUNCS = funcs


def _scan_calls(entries):
    new = set()
    for e in entries:
        for ins in discover(e, _FUNCS).values():
            new.update(t for t in ins.calls if t not in _FUNCS)
            # an address constant that points at cleanly decodable code is treated as a function pointer
            if ins.ptr is not None and ins.ptr not in _FUNCS and ins.ptr not in new and is_clean_code(ins.ptr, _FUNCS):
                new.add(ins.ptr)
    return new


def _emit_file(args):
    idx, entries, outdir = args
    stats = collections.Counter()
    unsup_words = collections.Counter()
    bad_funcs = []
    covered = set()
    parts = ['#include "recomp.h"', '#include "funcs.h"', '']
    for e in entries:
        body = discover(e, _FUNCS)
        stats['insns'] += len(body)
        bad = False
        for ins in body.values():
            if ins.unsup:
                stats['unsup:' + ins.unsup] += 1
                unsup_words[(ins.unsup, ins.w & 0x0FF00FF0)] += 1
                bad = True
        if bad:
            bad_funcs.append(e)
        parts.append(emit_function(e, body, _FUNCS))
        parts.append('')
        covered.update(body)
    with open(os.path.join(outdir, f'rc_{idx:03d}.c'), 'w') as fh:
        fh.write('\n'.join(parts))
    return stats, unsup_words, bad_funcs, covered


def load_game_file(root, title_id):
    """Hand-written per-game settings, games/<title id>.toml:
         [functions]  extra = [addresses...]     entries no scan found
                      not_code = [addresses...]  data that scans mistake for code
       Returns (extra, not_code) as sets; both empty when there is no file."""
    import tomllib
    path = layout_mod.find_game_file(root, title_id)
    if not path:
        return set(), set()
    with open(path, 'rb') as fh:
        t = tomllib.load(fh).get('functions', {})
    return set(t.get('extra', [])), set(t.get('not_code', []))


def main():
    img_path, fn_path, outdir = sys.argv[1:4]
    opts = dict(zip(sys.argv[4::2], sys.argv[5::2]))
    nfiles, jobs = int(opts.get('--files', 96)), int(opts.get('--jobs', os.cpu_count()))
    os.makedirs(outdir, exist_ok=True)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    lay = layout_mod.read_layout(os.path.join(os.path.dirname(img_path), 'exheader.bin'))
    img = open(img_path, 'rb').read()
    if len(img) != lay.image_size:
        sys.exit(f'{img_path} is {len(img):#x} bytes but the ExHeader describes {lay.image_size:#x}')
    layout_mod.write_layout_header(lay, os.path.join(outdir, 'layout.h'))

    # idioms: found by pattern, recorded so the result is visible
    nonlocal_returns = idioms.detect_inline_data_calls(img, lay.text_base, lay.text_base, lay.text_end)
    init_arrays = idioms.detect_relative_init_arrays(img, lay.text_base, lay.text_base, lay.text_end)
    specials = [{'kind': 'inline-data-call', 'target': f'0x{t:08x}', 'resume_offsets': list(o)} for t, o in sorted(nonlocal_returns.items())]
    specials += [{'kind': 'relative-init-array', 'range': [f'0x{a:08x}', f'0x{b:08x}']} for a, b in init_arrays]
    with open(os.path.join(outdir, 'specials.json'), 'w') as fh:
        json.dump({'title': lay.title_hex, 'name': lay.name, 'specials': specials}, fh, indent=1)
    if len(nonlocal_returns) > 1:
        print(f'note: the inline-data-call pattern matched {len(nonlocal_returns)} functions; check specials.json')
    configure(img, lay, nonlocal_returns)

    game_extra, not_code = load_game_file(root, lay.title_id)
    if opts.get('--extra') and os.path.exists(opts['--extra']):     # functions the runtime found missing (3dsrecomp run)
        game_extra |= {int(l.split()[0], 16) for l in open(opts['--extra']) if l.strip() and not l.startswith('#')}
    if fn_path == '-':                     # no list: discover every function from the entry point (analyze/discover.py)
        sys.path.insert(0, os.path.join(root, 'analyze'))
        import discover
        funcs, report = discover.discover_all(img_path, lay, nonlocal_returns, {lay.text_base} | game_extra, not_code, jobs)
        with open(os.path.join(outdir, 'discovery.txt'), 'w') as fh:
            fh.write('\n'.join(report) + '\n')
    else:
        funcs = {int(l, 16) for l in open(fn_path) if l.strip()}
        extra = os.path.join(os.path.dirname(fn_path), 'functions_extra.txt')
        if os.path.exists(extra):
            funcs |= {int(l.split()[0], 16) for l in open(extra) if l.strip() and not l.startswith('#')}
        funcs |= game_extra
    funcs -= not_code
    n_seed = len(funcs)
    _init(img_path, funcs, lay, nonlocal_returns)
    funcs = {f for f in funcs if in_text(f)}

    # pass 1: close the function set over direct call targets
    todo, rounds = sorted(funcs), 0
    while todo:
        rounds += 1
        chunks = [todo[i::jobs * 4] for i in range(jobs * 4)]
        with Pool(jobs, _init, (img_path, funcs, lay, nonlocal_returns)) as pool:
            new = set().union(*pool.map(_scan_calls, [c for c in chunks if c]))
        new -= funcs
        new -= not_code
        funcs |= new
        todo = sorted(new)
    order = sorted(funcs)
    print(f'functions: {n_seed} seeded, {len(order) - n_seed} added from call targets and code literals ({rounds} rounds)')

    # pass 2: emit
    per = (len(order) + nfiles - 1) // nfiles
    jobs_args = [(i, order[i * per:(i + 1) * per], outdir) for i in range(nfiles) if order[i * per:(i + 1) * per]]
    with Pool(jobs, _init, (img_path, funcs, lay, nonlocal_returns)) as pool:
        results = pool.map(_emit_file, jobs_args)
    stats, words, bad = collections.Counter(), collections.Counter(), []
    cov = bytearray((TEXT_END - BASE) // 4)
    for s, w, b, cv in results:
        stats.update(s); words.update(w); bad.extend(b)
        for a in cv:
            cov[(a - BASE) // 4] = 1
    open(os.path.join(outdir, 'covered.bin'), 'wb').write(cov)

    with open(os.path.join(outdir, 'funcs.h'), 'w') as fh:
        fh.write('#pragma once\n' + ''.join(f'void fn_{a:08x}(CPU *c);\n' for a in order))
    with open(os.path.join(outdir, 'table.c'), 'w') as fh:
        fh.write('#include "recomp.h"\n#include "funcs.h"\n')
        fh.write('const struct { uint32_t addr; recomp_fn fn; } recomp_functions[] = {\n')
        fh.write(''.join(f'  {{0x{a:08x}u, fn_{a:08x}}},\n' for a in order))
        fh.write(f'}};\nconst uint32_t recomp_function_count = {len(order)};\n')
    with open(os.path.join(outdir, 'functions_all.txt'), 'w') as fh:
        fh.write(''.join(f'{a:08x}\n' for a in order))
    with open(os.path.join(outdir, 'functions_unsupported.txt'), 'w') as fh:
        fh.write(''.join(f'{a:08x}\n' for a in sorted(bad)))

    total = stats['insns']
    un = sum(v for k, v in stats.items() if k.startswith('unsup:'))
    lines = [f'functions emitted: {len(order)}', f'instructions translated: {total}',
             f'unsupported instructions: {un} ({100.0 * un / max(total, 1):.3f}%)',
             f'functions containing an unsupported instruction: {len(bad)} ({100.0 * len(bad) / len(order):.2f}%)', '']
    for k, v in sorted(stats.items(), key=lambda kv: -kv[1]):
        if k.startswith('unsup:'):
            lines.append(f'  {k[6:]:24s} {v}')
    lines.append('\nmost common unsupported encodings (tag, word & 0x0ff00ff0):')
    for (tag, wm), v in words.most_common(25):
        lines.append(f'  {tag:24s} {wm:08x} x{v}')
    report = '\n'.join(lines)
    open(os.path.join(outdir, 'stats.txt'), 'w').write(report + '\n')
    print(report)


if __name__ == '__main__':
    main()
