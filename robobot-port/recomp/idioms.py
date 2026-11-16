"""Detectors for compiler/SDK idioms that plain control-flow analysis cannot follow.

Each detector matches instruction patterns, not addresses, so it works on other titles too.
Detectors report what they found; the caller records it (see armrecomp.py: build/gen/specials.json).
"""
import struct


def _words(img, base, start, end):
    """Yield (address, word) for 4-aligned words in [start, end)."""
    for a in range(start, end, 4):
        yield a, struct.unpack_from('<I', img, a - base)[0]


# ---- inline-data dispatcher ------------------------------------------------------------------
# A function whose first instruction is `ldr r4, [lr], #4` reads the word that follows its caller's
# `bl` (a descriptor) and then resumes the caller at one of the instructions after that word, not at
# the one right behind the `bl`. The ARM floating-point library's NaN/Inf dispatcher does this.
FPLIB_FIRST_WORD = 0xE49E4004
FPLIB_RESUME_OFFSETS = (8, 12, 16, 20)       # relative to the `bl`; offset 4 is the descriptor word


def detect_inline_data_calls(img, base, text_start, text_end):
    return {a: FPLIB_RESUME_OFFSETS for a, w in _words(img, base, text_start, text_end) if w == FPLIB_FIRST_WORD}


# ---- relative static-initialiser array -------------------------------------------------------
# The SDK start-up code walks a table of self-relative offsets (entry address + stored value = the
# constructor to call). The runner looks like this:
#       ldr  rA, [pc, #x]      ; add rA, pc, rA     (begin of table, position independent)
#       ldr  rB, [pc, #y]      ; add rB, pc, rB     (end of table)
#       b    test
#   loop:  ldr r0, [rA] ; add lr, pc, #4 ; add r0, r0, rA ; mov pc, r0
#          add rA, rA, #4
#   test:  cmp rA, rB ; bne loop
def _pc_relative_pair(img, base, addr, reg):
    """Find `ldr reg,[pc,#x]` followed by `add reg,pc,reg` shortly before addr; return the resolved address."""
    for back in range(1, 16):
        a = addr - 4 * back
        w = struct.unpack_from('<I', img, a - base)[0]
        if (w & 0xFF7FF000) == (0xE51F0000 | (reg << 12)) and (w >> 16) & 0xF == 15:     # ldr reg, [pc, #+/-x]
            lit = a + 8 + ((w & 0xFFF) if w & (1 << 23) else -(w & 0xFFF))
            if not (base <= lit < base + len(img) - 3):
                return None
            nxt = struct.unpack_from('<I', img, a + 4 - base)[0]
            if nxt == (0xE08F0000 | (reg << 12) | reg):                                    # add reg, pc, reg
                return (a + 4 + 8 + struct.unpack_from('<I', img, lit - base)[0]) & 0xFFFFFFFF
    return None


def detect_relative_init_arrays(img, base, text_start, text_end):
    found = []
    for a, w in _words(img, base, text_start, text_end - 16):
        if w != 0xE28FE004:                                                                # add lr, pc, #4
            continue
        ldr, add, mov = (struct.unpack_from('<I', img, a + k - base)[0] for k in (-4, 4, 8))
        if (ldr & 0xFFF00FFF) != 0xE5900000 or mov != 0xE1A0F000 or (add & 0xFFFFFFF0) != 0xE0800000:   # ldr r0,[rA]; add r0,r0,rA; mov pc,r0
            continue
        ra = (ldr >> 16) & 0xF
        if (add & 0xF) != ra:
            continue
        # the loop test `cmp rA, rB` follows the increment
        rb = None
        for k in range(12, 32, 4):
            c = struct.unpack_from('<I', img, a + k - base)[0]
            if (c & 0xFFF0FFF0) == 0xE1500000 and ((c >> 16) & 0xF) == ra:                # cmp rA, rB
                rb = c & 0xF
                break
        if rb is None:
            continue
        begin, end = _pc_relative_pair(img, base, a, ra), _pc_relative_pair(img, base, a, rb)
        if begin and end and begin < end:
            found.append((begin, end))
    return found
