#!/usr/bin/env python3
"""Write a fully decrypted NCCH (.cxi) of a CIA's main content, for loading in a reference emulator.

Handles the title-key CBC layer, the NCCH AES-CTR layers and seed crypto, then marks the NCCH as
NoCrypto. Usage: make_cxi.py <original.cia> <out.cxi> <seed-hex>
"""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import decrypt_cia as D
from Crypto.Cipher import AES
from Crypto.Util import Counter

u32 = lambda b, o=0: struct.unpack_from('<I', b, o)[0]
CH = 8 << 20


def ctr_inplace(f, off, size, key, iv):
    done = 0
    while done < size:
        n = min(CH, size - done)
        f.seek(off + done); data = f.read(n)
        f.seek(off + done)
        f.write(AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=iv + done // 16)).decrypt(data))
        done += n


def main(cia, out, seed_hex):
    seed = bytes.fromhex(seed_hex)
    with open(cia, 'rb') as f:
        h = f.read(0x20)
        a = D.align64
        tik = a(a(u32(h, 0)) + u32(h, 8)); tmd = a(tik + u32(h, 0xC)); con = a(tmd + u32(h, 0x10))
        f.seek(tik); t = f.read(0x300)
        ck = D.COMMON_KEYS.get(t[0x1F1], D.COMMON_KEYS[0])
        tkey = AES.new(ck, AES.MODE_CBC, iv=t[0x1DC:0x1E4] + bytes(8)).decrypt(t[0x1BF:0x1CF])
        contents, _ = D.parse_tmd(f, tmd)
        size = contents[0][3]
        cbc = AES.new(tkey, AES.MODE_CBC, iv=bytes(16))
        f.seek(con)
        with open(out, 'wb') as o:
            left = size
            while left:
                n = min(CH, left); o.write(cbc.decrypt(f.read(n))); left -= n
    with open(out, 'r+b') as f:
        n = f.read(0x200)
        assert n[0x100:0x104] == b'NCCH'
        flags = bytearray(n[0x188:0x190])
        tid = struct.unpack_from('<Q', n, 0x108)[0]
        keyy_b = n[:16]
        keyy = D.int128(struct.unpack('>QQ', keyy_b))
        sec_y = keyy
        if flags[7] & 0x20:
            assert hashlib.sha256(seed + struct.pack('<Q', tid)).digest()[:4] == n[0x114:0x118], 'seed does not match'
            sec_y = D.int128(struct.unpack('>QQ', hashlib.sha256(keyy_b + seed).digest()[:16]))
        k2c = D.to_bytes(D.derive_key(0x2C, keyy))
        ksec = D.to_bytes(D.derive_key(D.CRYPTO_MAP[flags[3]], sec_y))
        iv = lambda kind: int('%016X%016X' % ((tid,) + struct.unpack('>Q', bytes([kind]) + bytes(7))), 16)
        S = 0x200
        if u32(n, 0x180):
            ctr_inplace(f, S, 0x800, k2c, iv(1))
        exefs = u32(n, 0x1A0) * S
        if exefs:
            ctr_inplace(f, exefs, S, k2c, iv(2))
            f.seek(exefs); hdr = f.read(S)
            for i in range(10):
                e = hdr[i * 16:(i + 1) * 16]
                name, off, sz = e[:8].rstrip(b'\0'), u32(e, 8), u32(e, 12)
                if name and sz:
                    ctr_inplace(f, exefs + S + off, sz, ksec if name == b'.code' else k2c, iv(2) + (S + off) // 16)
        romfs, romfs_len = u32(n, 0x1B0) * S, u32(n, 0x1B4) * S
        if romfs:
            ctr_inplace(f, romfs, romfs_len, ksec, iv(3))
            f.seek(romfs); assert f.read(4) == b'IVFC', 'RomFS did not decrypt'
        flags[3] = 0
        flags[7] = (flags[7] & ~0x21) | 0x04                     # NoCrypto, no fixed key, no seed
        f.seek(0x188); f.write(bytes(flags))
    print(f'wrote {out}: title {tid:016x}, NoCrypto')


if __name__ == '__main__':
    main(*sys.argv[1:4])
