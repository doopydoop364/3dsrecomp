#!/usr/bin/env python3
"""Open replacements for system data archives a console always has, for when the user has not dumped theirs.

A dumped archive always wins: an existing file is never overwritten. Each replacement holds only what games need
to work, with no Nintendo data (the layout follows the citra_system_archives project used by Citra and Azahar):

  000400db00010302  NG word list: per-language lists that ban nothing real (only the pattern ^badword$)

Usage: make_open_sysarchives.py [<sysarchives folder>]   (default: system/sysarchives)
"""
import os, struct, sys


def _hash(parent, name):
    h = parent ^ 123456789
    for ch in (ord(c) for c in name):
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= ch
    return h


def _buckets(n):
    return 3 if n < 3 else n | 1


def build_romfs(files):
    """Level-3 RomFS (the part a game's file library reads) with every file in the root directory.
    files: list of (name, bytes), stored in the given order."""
    n = len(files)
    nb_d, nb_f = 3, _buckets(n)
    dir_meta = struct.pack('<6I', 0, 0xFFFFFFFF, 0xFFFFFFFF, 0 if n else 0xFFFFFFFF, 0xFFFFFFFF, 0)
    dir_hash = [0xFFFFFFFF] * nb_d
    dir_hash[_hash(0, '') % nb_d] = 0
    fmeta, offs, data = [], [], b''
    pos = 0
    for i, (name, content) in enumerate(files):
        offs.append(pos)
        data += content + (bytes((-len(content)) % 16) if i + 1 < n else b'')   # file data is 16-byte aligned
        pos = len(data)
    entry_off, fm = [], b''
    for name, content in files:
        entry_off.append(len(fm))
        nm = name.encode('utf-16le')
        fm += b'\0' * (0x20 + len(nm) + (-len(nm)) % 4)
    fm = bytearray(fm)
    file_hash = [0xFFFFFFFF] * nb_f
    for i, (name, content) in enumerate(files):
        nm = name.encode('utf-16le')
        b = _hash(0, name) % nb_f
        nxt = file_hash[b]
        file_hash[b] = entry_off[i]
        sib = entry_off[i + 1] if i + 1 < n else 0xFFFFFFFF
        struct.pack_into('<IIQQII', fm, entry_off[i], 0, sib, offs[i], len(content), nxt, len(nm))
        fm[entry_off[i] + 0x20:entry_off[i] + 0x20 + len(nm)] = nm
    dh, fh = struct.pack(f'<{nb_d}I', *dir_hash), struct.pack(f'<{nb_f}I', *file_hash)
    o_dh = 0x28
    o_dm = o_dh + len(dh)
    o_fh = o_dm + len(dir_meta)
    o_fm = o_fh + len(fh)
    o_data = (o_fm + len(fm) + 15) & ~15
    hdr = struct.pack('<10I', 0x28, o_dh, len(dh), o_dm, len(dir_meta), o_fh, len(fh), o_fm, len(fm), o_data)
    body = hdr + dh + dir_meta + fh + bytes(fm)
    return body + bytes(o_data - len(body)) + data


def ng_word_list():
    words = '﻿^badword$\n'.encode('utf-16le')            # matches nothing a player would type
    files = [(f'{i}.txt', words) for i in range(17)]
    files.sort(key=lambda f: f[0])
    files.append(('version.dat', struct.pack('<I', 0x12)))
    return build_romfs(files)


ARCHIVES = {'000400db00010302': ('NG word list', ng_word_list)}


def main():
    main_into(sys.argv[1] if len(sys.argv) > 1 else os.path.join('system', 'sysarchives'))


def main_into(out):
    os.makedirs(out, exist_ok=True)
    for tid, (what, make) in ARCHIVES.items():
        path = os.path.join(out, f'{tid}.romfs')
        if os.path.exists(path):
            continue
        with open(path, 'wb') as f:
            f.write(make())
        print(f'created an open replacement for the {what} ({tid}) in {path}')


if __name__ == '__main__':
    main()
