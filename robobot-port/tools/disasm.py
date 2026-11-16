#!/usr/bin/env python3
"""Disassemble a range of the game's code. Usage: disasm.py <start hex> [count]"""
import struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
import layout
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
d = open(os.path.join(root, 'game', 'code_decompressed.bin'), 'rb').read()
base = layout.read_layout(os.path.join(root, 'game', 'exheader.bin')).text_base
known = {int(l, 16) for l in open(os.path.join(root, 'build', 'gen', 'functions_all.txt'))}
a, n = int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 24
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for i in range(n):
    w = d[a - base + 4 * i: a - base + 4 * i + 4]
    ins = next(md.disasm(w, a + 4 * i), None)
    txt = f'{ins.mnemonic} {ins.op_str}' if ins else '.word'
    print(f'{"F" if a + 4 * i in known else " "} {a + 4 * i:08x}: {struct.unpack("<I", w)[0]:08x}  {txt}')
