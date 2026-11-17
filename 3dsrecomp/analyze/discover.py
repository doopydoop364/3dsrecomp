"""Function discovery without a hand-made list: from the entry point to a fixed point.

Each round:
  1. closure: recursive descent from every known function; direct call targets and address constants that point at
     cleanly decodable code become functions (armrecomp's own scan);
  2. stored pointers: every word that is not a known instruction (data, literal pools, gaps in .text) whose value is
     the address of cleanly decodable code (vtables, callbacks, member pointers);
  3. idioms: C++ static-initialiser tables (self-relative offsets), self-relative jump tables, targets of
     `add pc, rX, rTable` tables, code addresses built in two steps (`add rX, pc, #a; add rX, rX, #b`), and uncovered
     prologues that save the return address (`push {..., lr}`, `str lr, [sp, #-4]!`) anywhere in .text.
Rounds repeat until nothing new is found. "Cleanly decodable" means recursive descent from the address meets only
valid instructions, which keeps data that happens to point into .text out.
"""
import os, struct, sys
from multiprocessing import Pool
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
import armrecomp as R, idioms

_IMG = _COV = None


def _winit(img_path, lay, nlr, funcs, cov):
    global _IMG, _COV
    R._init(img_path, funcs, lay, nlr)
    _IMG, _COV = open(img_path, 'rb').read(), cov


def _closure_chunk(entries):
    """New call targets / code literals, and the addresses covered, for a chunk of functions."""
    new, cov = set(), []
    for e in entries:
        body = R.discover(e, R._FUNCS)
        cov.extend(body)
        for ins in body.values():
            new.update(t for t in ins.calls if t not in R._FUNCS)
            if ins.ptr is not None and ins.ptr not in R._FUNCS and ins.ptr not in new and R.is_clean_code(ins.ptr, R._FUNCS):
                new.add(ins.ptr)
    return new, cov


def _covered(a):
    off = (a - R.BASE) // 4
    return 0 <= off < len(_COV) and _COV[off]


def _words_chunk(rng):
    """Stored pointers in image words [lo, hi) (word offsets) that are not covered instructions."""
    lo, hi = rng
    out = set()
    for k in range(lo, hi):
        a = R.BASE + 4 * k
        if _covered(a):
            continue
        v = struct.unpack_from('<I', _IMG, 4 * k)[0]
        if R.in_text(v) and v not in R._FUNCS and v not in out and R.is_clean_code(v, R._FUNCS):
            out.add(v)
    return out


def _ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def _idioms_chunk(rng):
    """Pattern-based code addresses in .text word offsets [lo, hi): {source: set(addresses)}."""
    lo, hi = rng
    d, T0, T1 = _IMG, R.BASE, R.TEXT_END
    found = {'self-relative jump tables': set(), 'add-pc jump tables': set(), 'two-step addresses': set(), 'prologues': set()}
    ok = lambda t: R.in_text(t) and t not in R._FUNCS and R.is_clean_code(t, R._FUNCS)
    for off in range(4 * lo, min(4 * hi, T1 - T0 - 16), 4):
        w = struct.unpack_from('<I', d, off)[0]
        a = T0 + off
        if (w & 0x0FFF0000) in (0x024F0000, 0x028F0000):            # add/sub rX, pc, #imm -> table of negative offsets
            imm = _ror(w & 0xFF, ((w >> 8) & 0xF) * 2)
            t = a + 8 - imm if (w & 0x0FFF0000) == 0x024F0000 else a + 8 + imm
            if T0 <= t < T1 and not t & 3:
                entries = []
                for k in range(64):
                    v = struct.unpack_from('<i', d, t - T0 + 4 * k)[0]
                    if not (-0x4000 < v < 0) or (t + v) & 3:
                        break
                    entries.append(t + v)
                if len(entries) >= 2:
                    if a == t + 4 * len(entries):
                        entries.append(a)
                    found['self-relative jump tables'].update(e for e in entries if ok(e))
        if (w & 0x0FF0FFF0) == 0x0080F000 and (w >> 28) == 0xE:     # add pc, rX, rTable
            tab = R.selfrel_table(R.Ins(a, w))
            found['add-pc jump tables'].update(t for t in (tab[1] if tab else []) if ok(t))
        if (w & 0x0FEF0000) in (0x028F0000, 0x024F0000) and (w >> 28) == 0xE:   # add/sub rX, pc, #a; add/sub rX, rX, #b
            rd = (w >> 12) & 0xF
            imm = _ror(w & 0xFF, ((w >> 8) & 0xF) * 2)
            v = a + 8 + imm if (w & 0x00F00000) == 0x00800000 else a + 8 - imm
            for k in (1, 2, 3):
                w2 = struct.unpack_from('<I', d, off + 4 * k)[0]
                if (w2 & 0x0FE00000) in (0x02800000, 0x02400000) and ((w2 >> 16) & 0xF) == rd and ((w2 >> 12) & 0xF) == rd:
                    imm2 = _ror(w2 & 0xFF, ((w2 >> 8) & 0xF) * 2)
                    t = (v + imm2 if (w2 & 0x00E00000) == 0x00800000 else v - imm2) & 0xFFFFFFFF
                    if ok(t):
                        found['two-step addresses'].add(t)
                    break
                if ((w2 >> 12) & 0xF) == rd:
                    break
        # uncovered prologues that save the return address: push {..., lr} or str lr, [sp, #-4]!
        if off >= 4 and not _covered(a) and (((w & 0xFFFF4000) == 0xE92D4000) or w == 0xE52DE004) and ok(a):
            found['prologues'].add(a)
    return found


def discover_all(img_path, lay, nlr, seeds, not_code, jobs, log=print):
    """Returns (functions, report lines)."""
    img = open(img_path, 'rb').read()
    R.configure(img, lay, nlr)
    funcs = {f for f in seeds if R.in_text(f)} - not_code
    report = [f'seeds: {len(funcs)} (entry point and per-game extras)']
    cov = bytearray((R.TEXT_END - R.BASE) // 4)
    init_done = False
    for rnd in range(1, 100):
        added = {}
        # 1. closure over calls and code literals
        todo = sorted(funcs)
        while todo:
            with Pool(jobs, _winit, (img_path, lay, nlr, funcs, bytes(cov))) as pool:
                res = pool.map(_closure_chunk, [todo[i::jobs * 4] for i in range(jobs * 4) if todo[i::jobs * 4]])
            new = set()
            for n, c in res:
                new |= n
                for a in c:
                    if R.BASE <= a < R.TEXT_END:
                        cov[(a - R.BASE) // 4] = 1
            new -= funcs; new -= not_code
            added['calls and code literals'] = added.get('calls and code literals', 0) + len(new)
            funcs |= new
            todo = sorted(new)
        # 2+3. stored pointers and idioms, against the current function set and coverage
        nwords = len(img) // 4
        step = (nwords + jobs * 8 - 1) // (jobs * 8)
        tsteps = ((R.TEXT_END - R.BASE) // 4 + jobs * 8 - 1) // (jobs * 8)
        with Pool(jobs, _winit, (img_path, lay, nlr, funcs, bytes(cov))) as pool:
            ptrs = pool.map(_words_chunk, [(i, min(i + step, nwords)) for i in range(0, nwords, step)])
            idm = pool.map(_idioms_chunk, [(i, i + tsteps) for i in range(0, (R.TEXT_END - R.BASE) // 4, tsteps)])
        new = set().union(*ptrs) - funcs - not_code
        added['stored pointers'] = len(new)
        found = {}
        for part in idm:
            for k, v in part.items():
                found.setdefault(k, set()).update(v)
        if not init_done:                                          # static initialisers: self-relative offset tables
            inits = set()
            for lo, hi in idioms.detect_relative_init_arrays(img, lay.text_base, lay.text_base, lay.text_end):
                for a in range(lo, hi, 4):
                    v = (a + struct.unpack_from('<I', img, a - lay.text_base)[0]) & 0xFFFFFFFF
                    if R.in_text(v):
                        inits.add(v)
            found['static initialisers'] = inits
            init_done = True
        for k, v in found.items():
            v = v - funcs - not_code - new
            added[k] = len(v)
            new |= v
        funcs |= new
        line = f'round {rnd}: ' + ', '.join(f'{v} from {k}' for k, v in added.items() if v) + f' -> {len(funcs)} functions'
        report.append(line); log(line)
        if not new:
            break
    return funcs, report
