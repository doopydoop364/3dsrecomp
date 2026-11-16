#!/usr/bin/env python3
"""Fully decrypt + extract content 0 (the game NCCH) of an original CIA, with seed crypto.

Usage: extract_seeded.py <original.cia> <outdir> <seed-hex>
Reuses key tables/helpers from decrypt_cia.py next to it (import only, not modified).
"""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decrypt_cia as D
from Crypto.Cipher import AES
from Crypto.Util import Counter

from extract_ncch import blz_decode

u32 = lambda b, o=0: struct.unpack_from("<I", b, o)[0]

def ctr_stream(f, off, size, key, iv_int, out):
    """AES-CTR decrypt size bytes of f at off into file object out."""
    f.seek(off)
    done = 0
    while done < size:
        take = min(8 << 20, size - done)
        chunk = f.read(take)
        ctr = Counter.new(128, initial_value=iv_int + done // 16)
        out.write(AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(chunk))
        done += take

def main(cia, outdir, seed_hex):
    seed = bytes.fromhex(seed_hex)
    os.makedirs(outdir, exist_ok=False)
    tmp_ncch = os.path.join(outdir, "_content0.ncch")

    with open(cia, "rb") as f:
        h = f.read(0x20)
        hs, cs, ts, ms = u32(h, 0), u32(h, 8), u32(h, 0xC), u32(h, 0x10)
        a = D.align64
        tik = a(a(hs) + cs); tmd = a(tik + ts); con = a(tmd + ms)
        f.seek(tik); t = f.read(0x300)
        ck = D.COMMON_KEYS.get(t[0x1F1], D.COMMON_KEYS[0])
        tkey = AES.new(ck, AES.MODE_CBC, iv=t[0x1DC:0x1E4] + bytes(8)).decrypt(t[0x1BF:0x1CF])
        contents, _ = D.parse_tmd(f, tmd)
        cid, cidx, ctype, csz = contents[0]
        assert cidx == 0

        # 1) streaming CBC (title-key layer) decrypt of content 0 -> temp NCCH
        cipher = AES.new(tkey, AES.MODE_CBC, iv=struct.pack(">H", cidx) + bytes(14))
        f.seek(con)
        left = csz
        with open(tmp_ncch, "wb") as o:
            while left:
                take = min(8 << 20, left)
                o.write(cipher.decrypt(f.read(take)))
                left -= take
    print("CBC layer removed ->", tmp_ncch)

    with open(tmp_ncch, "rb") as f:
        n = f.read(0x200)
        assert n[0x100:0x104] == b"NCCH", "bad NCCH after CBC decrypt"
        flags = n[0x188:0x190]
        tid_le = struct.unpack_from("<Q", n, 0x108)[0]
        keyy_bytes = n[:16]
        keyy = D.int128(struct.unpack(">QQ", keyy_bytes))
        print("flags", flags.hex(), "title id", f"{tid_le:016X}")
        assert flags[7] & 0x20, "not a seed-crypto title"

        # seed sanity check: sha256(seed || titleid_le)[:4] == header bytes 0x114
        chk = hashlib.sha256(seed + struct.pack("<Q", tid_le)).digest()[:4]
        print("seed verify:", "OK" if chk == n[0x114:0x118] else f"MISMATCH ({chk.hex()} vs {n[0x114:0x118].hex()})")
        if chk != n[0x114:0x118]:
            sys.exit(1)

        seed_keyy = D.int128(struct.unpack(">QQ", hashlib.sha256(keyy_bytes + seed).digest()[:16]))
        key_2c = D.to_bytes(D.derive_key(0x2C, keyy))                      # exheader, ExeFS header, icon/banner
        keyx_id = D.CRYPTO_MAP[flags[3]]
        key_sec = D.to_bytes(D.derive_key(keyx_id, seed_keyy))             # .code, RomFS
        print(f"secondary KeyX slot {keyx_id:#x}")

        iv = lambda t: int("%016X%016X" % ((tid_le,) + D.struct.unpack(">Q", bytes([t]) + bytes(7))), 16)
        iv_plain, iv_exefs, iv_romfs = iv(1), iv(2), iv(3)
        S = 0x200
        exh_len = u32(n, 0x180)
        exefs_off, exefs_len = u32(n, 0x1A0) * S, u32(n, 0x1A4) * S
        romfs_off, romfs_len = u32(n, 0x1B0) * S, u32(n, 0x1B4) * S

        # ExHeader
        f.seek(S)
        exh = AES.new(key_2c, AES.MODE_CTR, counter=Counter.new(128, initial_value=iv_plain)).decrypt(f.read(0x800))
        open(os.path.join(outdir, "exheader.bin"), "wb").write(exh)
        compressed = bool(exh[0xD] & 1)
        text_sz, ro_sz, data_sz = u32(exh, 0x18), u32(exh, 0x28), u32(exh, 0x38)
        print("title name:", exh[:8].rstrip(bytes(1)), "code compressed:", compressed)

        # ExeFS header + files
        f.seek(exefs_off)
        hdr = AES.new(key_2c, AES.MODE_CTR, counter=Counter.new(128, initial_value=iv_exefs)).decrypt(f.read(S))
        for i in range(10):
            e = hdr[i * 16:(i + 1) * 16]
            name = e[:8].rstrip(bytes(1)).decode("ascii", "replace")
            off, size = u32(e, 8), u32(e, 12)
            if not name or not size:
                continue
            file_off_in_exefs = S + off
            ivf = iv_exefs + file_off_in_exefs // 16
            # .code uses the secondary key; everything else uses the primary 0x2C key
            key = key_sec if name == ".code" else key_2c
            with open(os.path.join(outdir, name.lstrip(".") + ".bin"), "wb") as o:
                ctr_stream(f, exefs_off + file_off_in_exefs, size, key, ivf, o)
            print(f"ExeFS {name}: {size:#x} bytes (key {'secondary' if key is key_sec else 'primary'})")

        # RomFS
        if romfs_off:
            f.seek(romfs_off)
            first = AES.new(key_sec, AES.MODE_CTR, counter=Counter.new(128, initial_value=iv_romfs)).decrypt(f.read(16))
            print("RomFS magic:", first[:4])
            assert first[:4] == b"IVFC", "RomFS key wrong"
            with open(os.path.join(outdir, "romfs.bin"), "wb") as o:
                ctr_stream(f, romfs_off, romfs_len, key_sec, iv_romfs, o)
            print("RomFS saved")

    os.remove(tmp_ncch)

    # decompress .code and sanity-check against ExHeader
    raw = open(os.path.join(outdir, "code.bin"), "rb").read()
    dec = blz_decode(raw) if compressed else raw
    expect = text_sz + ro_sz + data_sz
    open(os.path.join(outdir, "code_decompressed.bin"), "wb").write(dec)
    print(f".code decompressed: {len(dec):#x}, ExHeader expects {expect:#x} ->", "MATCH" if len(dec) == expect else "MISMATCH")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
