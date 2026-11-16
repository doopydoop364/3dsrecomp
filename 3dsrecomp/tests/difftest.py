#!/usr/bin/env python3
"""Differential test: run guest functions in a reference ARM11 emulator (Unicorn) and in the
recompiled native build, on identical inputs, and compare the resulting CPU and memory state.

Unicorn is only a test oracle here; the port itself does not use an emulator.

Usage: difftest.py [--limit N] [--variants N] [--fn ADDR] [--jobs N] [--seed N]
"""
import argparse, collections, os, random, struct, subprocess, sys, zlib
from multiprocessing import Pool

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
import layout
IMG_PATH = os.path.join(ROOT, 'game', 'code_decompressed.bin')
LAY = layout.read_layout(os.path.join(ROOT, 'game', 'exheader.bin'))
IMG_BASE, RO_END, RW_END, TEXT_END = LAY.text_base, LAY.ro_end, LAY.rw_end, LAY.text_end
SCRATCH_BASE, SCRATCH_SIZE = 0x08000000, 0x10000
STACK_BASE, STACK_SIZE = 0x0FFE0000, 0x10000
RET_MAGIC = 0x1FF00000
NPAT = 6
INSN_LIMIT = 200000
IN_FMT, OUT_FMT = '<2I13II32I', '<I15II32II3I'
IN_SIZE, OUT_SIZE = struct.calcsize(IN_FMT), struct.calcsize(OUT_FMT)


def gen_pattern(p):
    x, kind, out = (0x9E3779B9 * (p + 1)) & 0xFFFFFFFF, p % 3, []
    for _ in range(SCRATCH_SIZE // 4):
        x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF
        out.append(x if kind == 0 else SCRATCH_BASE + ((x % (SCRATCH_SIZE - 0x400)) & ~3) if kind == 1 else x & 0xFF)
    return struct.pack(f'<{len(out)}I', *out)


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def make_input(fn, variant, seed):
    rng = random.Random((fn << 8) ^ variant ^ (seed << 40))

    def arg():
        k = rng.random()
        if k < 0.5:
            return SCRATCH_BASE + (rng.randrange(0, 0xE000) & ~3)
        return rng.randrange(0, 64) if k < 0.8 else rng.getrandbits(32)

    def fp():
        k = rng.random()
        if k < 0.5:
            return f32(rng.uniform(-1000.0, 1000.0))
        if k < 0.8:
            return f32(float(rng.randrange(-16, 17)))
        return f32(rng.choice([0.0, 1.0, -1.0, 0.5, 1e-3, 1e6])) if k < 0.92 else rng.getrandbits(32)

    r = [arg() for _ in range(4)] + [arg() if rng.random() < 0.3 else rng.getrandbits(32) for _ in range(9)]
    return (fn, variant % NPAT, *r, rng.getrandbits(4), *[fp() for _ in range(32)])


# ------------------------------------------------------------------ reference side
_uc = None


def _init_worker():
    global _uc, _rw, _pats, _regs
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_INTR, UC_PROT_READ, UC_PROT_EXEC, UC_PROT_WRITE
    from unicorn import arm_const as A
    img = open(IMG_PATH, 'rb').read()
    img += bytes(RW_END - IMG_BASE - len(img))
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_11MPCORE)
    uc.mem_map(IMG_BASE, RO_END - IMG_BASE, UC_PROT_READ | UC_PROT_EXEC)
    uc.mem_write(IMG_BASE, img[:RO_END - IMG_BASE])
    uc.mem_map(RO_END, RW_END - RO_END, UC_PROT_READ | UC_PROT_WRITE)
    uc.mem_map(SCRATCH_BASE, SCRATCH_SIZE, UC_PROT_READ | UC_PROT_WRITE)
    uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
    uc.mem_map(RET_MAGIC, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(A.UC_ARM_REG_C13_C0_3, SCRATCH_BASE + 0x8000)
    uc.hook_add(UC_HOOK_INTR, _on_intr)
    _uc, _rw = uc, img[RO_END - IMG_BASE:]
    _pats = [gen_pattern(p) for p in range(NPAT)]
    _regs = ([getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR],
             [getattr(A, f'UC_ARM_REG_D{i}') for i in range(16)], A)


_intr = [False]


def _on_intr(uc, intno, data):
    _intr[0] = True
    uc.emu_stop()


def _reference(tests):
    """Returns one packed TestOut (or None when the reference run did not finish cleanly) per test."""
    from unicorn import UcError
    uc, (gp, dreg, A) = _uc, _regs
    res = []
    zeros = bytes(STACK_SIZE)
    for t in tests:
        fn, pat, r, flags, s = t[0], t[1], t[2:15], t[15], t[16:48]
        uc.mem_write(RO_END, _rw)
        uc.mem_write(SCRATCH_BASE, _pats[pat])
        uc.mem_write(STACK_BASE, zeros)
        for i in range(13):
            uc.reg_write(gp[i], r[i])
        uc.reg_write(gp[13], STACK_BASE + STACK_SIZE - 0x1000)
        uc.reg_write(gp[14], RET_MAGIC)
        uc.reg_write(A.UC_ARM_REG_CPSR, (flags << 28) | 0x1F)
        for i in range(16):
            uc.reg_write(dreg[i], s[2 * i] | (s[2 * i + 1] << 32))
        uc.reg_write(A.UC_ARM_REG_FPSCR, 0)
        _intr[0] = False
        try:
            uc.emu_start(fn, RET_MAGIC, count=INSN_LIMIT)
        except UcError:
            res.append(None); continue
        cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
        if _intr[0] or uc.reg_read(A.UC_ARM_REG_PC) != RET_MAGIC or (cpsr & 0x3F) != 0x1F:
            res.append(None); continue
        regs = [uc.reg_read(g) for g in gp]
        sv = []
        for i in range(16):
            d = uc.reg_read(dreg[i])
            sv += [d & 0xFFFFFFFF, d >> 32]
        res.append(struct.pack(OUT_FMT, 0, *regs, cpsr >> 28, *sv, uc.reg_read(A.UC_ARM_REG_FPSCR),
                               zlib.crc32(uc.mem_read(SCRATCH_BASE, SCRATCH_SIZE)),
                               zlib.crc32(uc.mem_read(STACK_BASE, STACK_SIZE)),
                               zlib.crc32(uc.mem_read(RO_END, RW_END - RO_END))))
    return res


FIELDS = ['status'] + [f'r{i}' for i in range(13)] + ['sp', 'lr', 'flags'] + [f's{i}' for i in range(32)] + \
         ['fpscr', 'mem_scratch', 'mem_stack', 'mem_data']
FPSCR_MASK = 0xF7C00000          # condition flags and control bits; cumulative exception bits are not modelled


def diff(ref, nat):
    a, b = list(struct.unpack(OUT_FMT, ref)), list(struct.unpack(OUT_FMT, nat))
    i = FIELDS.index('fpscr')
    a[i] &= FPSCR_MASK; b[i] &= FPSCR_MASK
    return [(FIELDS[k], a[k], b[k]) for k in range(len(a)) if a[k] != b[k]]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--variants', type=int, default=3)
    ap.add_argument('--fn', action='append', default=[])
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=25)
    a = ap.parse_args()
    gen = os.path.join(ROOT, 'build', 'gen')
    if a.fn:
        fns = [int(x, 16) for x in a.fn]
    else:
        bad = {int(l, 16) for l in open(os.path.join(gen, 'functions_unsupported.txt'))}
        fns = [f for f in (int(l, 16) for l in open(os.path.join(gen, 'functions_all.txt'))) if f not in bad]
        if a.limit:
            fns = random.Random(a.seed).sample(fns, min(a.limit, len(fns)))
    tests = [make_input(f, v, a.seed) for f in fns for v in range(a.variants)]
    print(f'{len(fns)} functions, {len(tests)} test runs')

    chunks = [tests[i:i + 256] for i in range(0, len(tests), 256)]
    with Pool(a.jobs, _init_worker) as pool:
        ref = [r for part in pool.map(_reference, chunks) for r in part]

    vec, out = os.path.join(ROOT, 'build', 'vectors.bin'), os.path.join(ROOT, 'build', 'results.bin')
    # only runs the reference completed are worth executing natively
    keep = [i for i in range(len(tests)) if ref[i] is not None]
    tests, ref = [tests[i] for i in keep], [ref[i] for i in keep]
    n_total = len(chunks) and sum(len(ch) for ch in chunks)
    with open(vec, 'wb') as fh:
        for t in tests:
            fh.write(struct.pack(IN_FMT, *t))
    log = subprocess.run([os.path.join(ROOT, 'build', 'runner'), IMG_PATH, vec, out], capture_output=True, text=True)
    if log.returncode:
        print('runner failed:', log.returncode, log.stderr[-2000:]); sys.exit(1)
    nat = open(out, 'rb').read()
    why, missing = {}, set()
    for line in log.stderr.splitlines():
        p = line.split(' ', 2)
        why.setdefault(int(p[0], 16), p[2] if len(p) > 2 else '')
        # an unknown jump/call target inside .text is a function the recompiler has not seen yet
        if 'unexpected address' in line or 'unknown function' in line:
            for tok in line.split():
                for pre in ('info=', '@'):
                    if tok.startswith(pre):
                        v = int(tok[len(pre):], 16)
                        if IMG_BASE <= v < TEXT_END and not v & 3 and ('unknown function' in line) == (pre == '@'):
                            missing.add(v)

    valid = match = 0
    fn_ok, fn_bad = set(), {}
    native_fail = collections.Counter()
    for i, t in enumerate(tests):
        if ref[i] is None:
            continue
        valid += 1
        n = nat[i * OUT_SIZE:(i + 1) * OUT_SIZE]
        d = diff(ref[i], n)
        if not d:
            match += 1; fn_ok.add(t[0])
        else:
            if struct.unpack_from('<I', n)[0]:
                native_fail[why.get(t[0], '?').split(' @')[0]] += 1
            fn_bad.setdefault(t[0], (t[1], d))
    fn_ok -= set(fn_bad)
    print(f'reference runs that completed: {valid} of {n_total}')
    print(f'matching runs: {match} of {valid} ({100.0 * match / max(valid, 1):.2f}%)')
    print(f'functions verified (every completed run matches): {len(fn_ok)}')
    print(f'functions with a mismatch: {len(fn_bad)}')
    if native_fail:
        print('native-side failures by reason:', dict(native_fail.most_common(8)))
    for fn, (pat, d) in sorted(fn_bad.items())[:a.show]:
        txt = ', '.join(f'{k}: ref {x:08x} got {y:08x}' for k, x, y in d[:5])
        print(f'  {fn:08x} pattern {pat}: {txt}{" ..." if len(d) > 5 else ""}  {why.get(fn, "")}')
    known = {int(l, 16) for l in open(os.path.join(gen, 'functions_all.txt'))}
    missing -= known
    if missing:
        print(f'{len(missing)} jump/call targets are not known functions (written to build/missing_functions.txt)')
    with open(os.path.join(ROOT, 'build', 'missing_functions.txt'), 'w') as fh:
        fh.write(''.join(f'{f:08x} # found by difftest\n' for f in sorted(missing)))
    with open(os.path.join(ROOT, 'build', 'mismatch.txt'), 'w') as fh:
        fh.write(''.join(f'{f:08x}\n' for f in sorted(fn_bad)))
    with open(os.path.join(ROOT, 'build', 'verified.txt'), 'w') as fh:
        fh.write(''.join(f'{f:08x}\n' for f in sorted(fn_ok)))


if __name__ == '__main__':
    main()
