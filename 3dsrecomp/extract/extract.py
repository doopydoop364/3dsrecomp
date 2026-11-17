#!/usr/bin/env python3
"""3dsrecomp extract: one game dump -> a game folder the recompiler and runtime use.

Accepts a CIA (encrypted or already decrypted), a cartridge image (.3ds / .cci) or a bare NCCH (.cxi). Writes
exheader.bin, code.bin, code_decompressed.bin, icon.bin, banner.bin, romfs.bin and title.json.

Decryption is done on the fly with random-access reads (no temporary copy of the game):
  - CIA title-key layer: AES-CBC with the title key from the ticket (common keys from decrypt_cia.py);
  - NCCH layer: AES-CTR; ExHeader, ExeFS header, icon and banner use key slot 0x2C, .code and RomFS the slot named
    by the header (0x2C, 0x25, 0x18 or 0x1B), with the KeyY replaced by SHA-256(KeyY || seed) for seed-encrypted games.
Seeds come from seeddb.bin (--seeddb, else seeddb.bin in the 3dsrecomp folder, else in the current folder) or, with
--fetch-seeds, from Nintendo's seed server. Every seed is checked against the hash in the game's header before use,
so a wrong or tampered seed is rejected.
"""
import argparse, contextlib, hashlib, io, json, os, ssl, struct, sys, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decrypt_cia as D
from extract_ncch import blz_decode
from Crypto.Cipher import AES
from Crypto.Util import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIA = 0x200
CHUNK = 8 << 20
u32 = lambda b, o=0: struct.unpack_from("<I", b, o)[0]


class Plain:
    """Bytes of a file region."""
    def __init__(self, f, base, size):
        self.f, self.base, self.size = f, base, size
    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)


class CbcLayer:
    """A CIA content encrypted with the title key (AES-CBC), decrypted on demand at any offset."""
    def __init__(self, f, base, size, key, iv):
        self.f, self.base, self.size, self.key, self.iv = f, base, size, key, iv
    def read(self, off, n):
        first, last = off // 16, (off + n + 15) // 16
        self.f.seek(self.base + max(first - 1, 0) * 16)
        prev = self.iv if first == 0 else self.f.read(16)
        data = self.f.read((last - first) * 16)
        out = AES.new(self.key, AES.MODE_CBC, iv=prev).decrypt(data)
        return out[off - first * 16: off - first * 16 + n]


def ctr_read(src, off, n, key, iv):
    """n bytes at off of an AES-CTR region whose counter starts at iv (at src offset 0 of the region)."""
    if key is None:
        return src.read(off, n)
    blk = off // 16
    data = src.read(blk * 16, (off - blk * 16) + n)
    ctr = Counter.new(128, initial_value=iv + blk)
    return AES.new(key, AES.MODE_CTR, counter=ctr).decrypt(data)[off - blk * 16:]


def ctr_copy(src, off, n, key, iv, out):
    """Decrypt n bytes of a CTR region starting at src offset off (counter iv at off) into a file."""
    done = 0
    while done < n:
        take = min(CHUNK, n - done)
        data = src.read(off + done, take)
        if key is not None:
            data = AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=iv + done // 16)).decrypt(data)
        out.write(data)
        done += take


def seed_ok(seed, tid, check):
    return hashlib.sha256(seed + struct.pack("<Q", tid)).digest()[:4] == check


def find_seed(tid, check, seeddb, fetch):
    paths = [p for p in (seeddb, os.path.join(ROOT, "seeddb.bin"), "seeddb.bin") if p]
    for p in paths:
        if not os.path.exists(p):
            continue
        d = open(p, "rb").read()
        for i in range(u32(d, 0)):
            if struct.unpack_from("<Q", d, 16 + 32 * i)[0] == tid:
                seed = d[16 + 32 * i + 8: 16 + 32 * i + 24]
                if seed_ok(seed, tid, check):
                    print(f"seed: from {p}")
                    return seed
                print(f"seed: the entry in {p} does not match this game, ignored")
    if fetch:
        # Nintendo's seed server presents a certificate from Nintendo's own authority, which this system cannot
        # verify; the seed is instead verified against the game header (seed_ok) before it is used.
        ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        for country in ("US", "JP", "GB", "KR", "TW", "AU", "NZ"):
            url = f"https://kagiya-ctr.cdn.nintendo.net/title/0x{tid:016X}/ext_key?country={country}"
            try:
                seed = urllib.request.urlopen(url, context=ctx, timeout=15).read()
            except OSError:
                continue
            if len(seed) == 16 and seed_ok(seed, tid, check):
                print(f"seed: fetched from Nintendo's seed server ({country})")
                return seed
    return None


def open_ncch(path, f):
    """Returns (NCCH reader, how it was found)."""
    f.seek(0)
    head = f.read(0x200)
    size = os.path.getsize(path)
    if head[0x100:0x104] == b"NCCH":
        return Plain(f, 0, size), "NCCH"
    if head[0x100:0x104] == b"NCSD":                               # cartridge image: partition 0 is the game
        off, sz = u32(head, 0x120) * MEDIA, u32(head, 0x124) * MEDIA
        return Plain(f, off, sz), "cartridge image (NCSD)"
    if u32(head, 0) == 0x2020:                                     # CIA: header, certs, ticket, TMD, contents
        hs, cs, ts, ms = u32(head, 0), u32(head, 8), u32(head, 0xC), u32(head, 0x10)
        a = D.align64
        tik = a(a(hs) + cs); tmd = a(tik + ts); con = a(tmd + ms)
        f.seek(tik); t = f.read(0x300)
        with contextlib.redirect_stdout(io.StringIO()):            # parse_tmd prints a line per content
            contents, _ = D.parse_tmd(f, tmd)
        cid, cidx, ctype, csz = contents[0]
        if not ctype & 1:
            return Plain(f, con, csz), "CIA (content not encrypted)"
        ck = D.COMMON_KEYS.get(t[0x1F1], D.COMMON_KEYS[0])
        tkey = AES.new(ck, AES.MODE_CBC, iv=t[0x1DC:0x1E4] + bytes(8)).decrypt(t[0x1BF:0x1CF])
        return CbcLayer(f, con, csz, tkey, struct.pack(">H", cidx) + bytes(14)), "CIA (title-key encrypted)"
    sys.exit(f"{path}: not a CIA, cartridge image or NCCH")


def smdh_names(icon):
    """English short title, long title and publisher from an SMDH icon file."""
    if icon[:4] != b"SMDH":
        return {}
    e = 8 + 1 * 0x200                                              # language 1 = English
    s = lambda o, n: icon[e + o: e + o + n].decode("utf-16-le", "replace").split("\0")[0].strip()
    return {"title": s(0, 0x80), "long_title": s(0x80, 0x100).replace("\n", " "), "publisher": s(0x180, 0x80)}


def extract(path, out, seeddb=None, fetch=False, force=False):
    f = open(path, "rb")
    src, kind = open_ncch(path, f)
    n = src.read(0, 0x200)
    if n[0x100:0x104] != b"NCCH":
        sys.exit("the game partition is not a valid NCCH (wrong key or damaged dump)")
    flags = n[0x188:0x190]
    tid = struct.unpack_from("<Q", n, 0x108)[0]
    product = n[0x150:0x160].rstrip(b"\0").decode("ascii", "replace")
    print(f"{path}: {kind}, title {tid:016x}, product {product}")
    if os.path.exists(out) and os.listdir(out) and not force:
        sys.exit(f"{out} is not empty (use --force to overwrite)")
    os.makedirs(out, exist_ok=True)

    # keys
    if flags[7] & 0x04:
        key_main = key_sec = None
        print("encryption: none")
    else:
        keyy = D.int128(struct.unpack(">QQ", n[:16]))
        if flags[7] & 0x01:                                        # fixed key (development/system titles)
            key_main = key_sec = bytes(16)
            print("encryption: fixed key")
        else:
            key_main = D.to_bytes(D.derive_key(0x2C, keyy))
            sec_keyy = keyy
            if flags[7] & 0x20:
                seed = find_seed(tid, n[0x114:0x118], seeddb, fetch)
                if seed is None:
                    sys.exit("this game uses seed encryption and its seed was not found: put seeddb.bin in the 3dsrecomp folder "
                             "(or pass --seeddb), or use --fetch-seeds")
                sec_keyy = D.int128(struct.unpack(">QQ", hashlib.sha256(n[:16] + seed).digest()[:16]))
            slot = D.CRYPTO_MAP.get(flags[3])
            if slot is None:
                sys.exit(f"unknown NCCH crypto method {flags[3]:#x}")
            key_sec = D.to_bytes(D.derive_key(slot, sec_keyy))
            print(f"encryption: key slot 0x2C, secondary {slot:#x}{' with seed' if flags[7] & 0x20 else ''}")

    iv = lambda t: (tid << 64) | (t << 56)
    exh_len = u32(n, 0x180)
    exefs_off, exefs_len = u32(n, 0x1A0) * MEDIA, u32(n, 0x1A4) * MEDIA
    romfs_off, romfs_len = u32(n, 0x1B0) * MEDIA, u32(n, 0x1B4) * MEDIA
    if not exh_len or not exefs_off:
        sys.exit("this NCCH has no ExHeader/ExeFS (not an application)")

    def region(off):                                               # reader for one NCCH region (counters start at 0)
        class R:
            def read(self, o, k): return src.read(off + o, k)
        return R()

    exh = ctr_read(region(MEDIA), 0, 0x800, key_main, iv(1))
    open(os.path.join(out, "exheader.bin"), "wb").write(exh)
    compressed = bool(exh[0xD] & 1)

    exefs = region(exefs_off)
    hdr = ctr_read(exefs, 0, MEDIA, key_main, iv(2))
    names = {}
    for i in range(10):
        e = hdr[i * 16:(i + 1) * 16]
        name = e[:8].rstrip(b"\0").decode("ascii", "replace")
        off, size = u32(e, 8), u32(e, 12)
        if not name or not size:
            continue
        key = key_sec if name == ".code" else key_main
        fname = name.lstrip(".") + ".bin"
        with open(os.path.join(out, fname), "wb") as o:
            ctr_copy(exefs, MEDIA + off, size, key, iv(2) + (MEDIA + off) // 16, o)
        names[name] = size
    if ".code" not in names:
        sys.exit("ExeFS has no .code")

    if romfs_off:
        romfs = region(romfs_off)
        if ctr_read(romfs, 0, 4, key_sec, iv(3)) != b"IVFC":
            if key_sec is None:
                sys.exit("this dump is marked as decrypted, but its RomFS is still encrypted: a decryptor without the game's "
                         "seed produced it. Use the original (encrypted) dump instead; 3dsrecomp decrypts it itself.")
            sys.exit("RomFS does not decrypt to IVFC (wrong key or seed)")
        with open(os.path.join(out, "romfs.bin"), "wb") as o:
            ctr_copy(romfs, 0, romfs_len, key_sec, iv(3), o)

    raw = open(os.path.join(out, "code.bin"), "rb").read()
    code = blz_decode(raw) if compressed else raw
    expect = (u32(exh, 0x14) + u32(exh, 0x24) + u32(exh, 0x34)) * 0x1000      # text, rodata, data: whole pages
    if len(code) != expect:
        sys.exit(f".code decompressed to {len(code):#x} bytes, the ExHeader expects {expect:#x}")
    open(os.path.join(out, "code_decompressed.bin"), "wb").write(code)

    icon = open(os.path.join(out, "icon.bin"), "rb").read() if os.path.exists(os.path.join(out, "icon.bin")) else b""
    info = {"title_id": f"{tid:016x}", "product_code": product, **smdh_names(icon)}
    json.dump(info, open(os.path.join(out, "title.json"), "w"), indent=2, ensure_ascii=False)
    print(f"extracted {info.get('title') or product} to {out}: ExeFS {', '.join(sorted(names))}"
          f"{', RomFS %d MB' % (romfs_len >> 20) if romfs_off else ''}")
    return info


def main(argv=None):
    ap = argparse.ArgumentParser(prog="3dsrecomp extract", description="Decrypt and unpack a 3DS game dump into a game folder.")
    ap.add_argument("dump", help=".cia, .3ds/.cci or .cxi file")
    ap.add_argument("--out", help="output folder (default: work/<title id>/game in the 3dsrecomp folder)")
    ap.add_argument("--seeddb", help="seeddb.bin to use for seed-encrypted games")
    ap.add_argument("--fetch-seeds", action="store_true", help="ask Nintendo's seed server when seeddb.bin has no seed")
    ap.add_argument("--force", action="store_true", help="overwrite a non-empty output folder")
    a = ap.parse_args(argv)
    out = a.out
    if not out:                                                    # peek at the title id for the default folder
        with open(a.dump, "rb") as f:
            src, _ = open_ncch(a.dump, f)
            tid = struct.unpack_from("<Q", src.read(0, 0x200), 0x108)[0]
        out = os.path.join(ROOT, "work", f"{tid:016x}", "game")
    extract(a.dump, out, a.seeddb, a.fetch_seeds, a.force)


if __name__ == "__main__":
    main()
