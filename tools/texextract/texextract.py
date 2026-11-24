"""Offline texture dump: every 2D texture in the game's CPK packs -> <data>/textures/dump, named and hashed exactly as
tms.exe's NX_TEX_DUMP=1 does (<w>x<h>_<nvn fmt>_<hash>.dds, level 0), so tools/uhd_pack.py can build a full UHD pack
without playing through the game.

    python tools/texextract/texextract.py <data> [--packs pack_000_map,pack_010_character] [--out DIR] [--list FILE]

Textures live in BNTX containers (Interface .gtx files, and inside .apak / .bfres / .ptcl / .pspk files): every
"BNTX" found in a file is parsed. Mods are applied like tms.exe does (<data>/mods/<name>/romfs/Pack/<pack>.cpk
replaces the pack, first enabled mod by name; settings.ini mods_disabled is honoured), so hashes match what the game
loads. Needs fasttex.dll next to this file (clang -O2 -shared -o fasttex.dll fasttex.c); on Linux fasttex.so
(clang -O2 -shared -fPIC -o fasttex.so fasttex.c).
"""
import argparse
import ctypes
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from cpk import Cpk  # noqa: E402

dll = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "fasttex.so" if sys.platform.startswith("linux") else "fasttex.dll"))
dll.tex_hash.restype = ctypes.c_uint64
dll.tex_hash.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
dll.untile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
dll.gob_block_height.restype = ctypes.c_uint32
dll.level_size.restype = ctypes.c_uint32
dll.crilayla.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]

# BNTX format (type << 8 | variant) -> (nvn format, DXGI, bytes per 4x4 block or 0 for 4 bytes per pixel). Only the
# formats tms.exe hashes (vk_texture.cpp k_dxgi); variant 1 UNORM, 6 SRGB.
FORMATS = {
    0x0b01: (0x25, 28, 0), 0x0b06: (0x38, 29, 0), 0x0c01: (0x30, 87, 0), 0x0c06: (0x39, 91, 0),
    0x1a01: (0x42, 71, 8), 0x1a06: (0x46, 72, 8), 0x1b01: (0x43, 74, 16), 0x1b06: (0x47, 75, 16),
    0x1c01: (0x44, 77, 16), 0x1c06: (0x48, 78, 16), 0x1d01: (0x49, 80, 8), 0x1e01: (0x4b, 83, 16),
    0x2001: (0x4d, 98, 16), 0x2006: (0x4e, 99, 16),
}


def read_entry(c, r):
    c.f.seek(r["_offset"])
    data = c.f.read(r["FileSize"])
    if r["ExtractSize"] != r["FileSize"] and data[:8] == b"CRILAYLA":
        usize = struct.unpack_from("<I", data, 8)[0]
        out = ctypes.create_string_buffer(usize + 0x100)
        if not dll.crilayla(data, len(data), out, len(out)):
            raise ValueError(c.path(r) + ": bad CRILAYLA data")
        data = out.raw
    return data


def dds(w, h, dxgi, data):
    hdr = [0] * 32
    hdr[:8] = [0x20534444, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, len(data), 0, 1]
    hdr[19], hdr[20], hdr[21], hdr[27] = 32, 0x4, 0x30315844, 0x1000
    return struct.pack("<32I", *hdr) + struct.pack("<5I", dxgi, 3, 0, 1, 0) + data


def bntx_textures(buf, base):
    """(w, h, nvn fmt, dxgi, level-0 linear bytes) for each 2D block-linear texture of the BNTX at buf[base:]."""
    if buf[base + 0x20:base + 0x24] != b"NX  ":
        return
    count, info_ptr = struct.unpack_from("<IQ", buf, base + 0x24)
    for i in range(count):
        brti = base + struct.unpack_from("<Q", buf, base + info_ptr + 8 * i)[0]
        if buf[brti:brti + 4] != b"BRTI":
            continue
        _, _, tile, _, mips, _, fmt, _, w, h, depth, layers, bhl = struct.unpack_from("<BBHHHIIIIIIII", buf, brti + 16)
        tex_type = struct.unpack_from("<I", buf, brti + 0x5C)[0]
        mip_ptrs = struct.unpack_from("<Q", buf, brti + 0x70)[0]
        if tile != 0 or tex_type != 1 or layers != 1 or depth > 1 or fmt not in FORMATS:
            continue
        nvn, dxgi, block = FORMATS[fmt]
        level0 = base + struct.unpack_from("<Q", buf, base + mip_ptrs)[0]
        bw = 4 if block else 1
        bx, by = (w + bw - 1) // bw, (h + bw - 1) // bw
        row = bx * (block or 4)
        bh = dll.gob_block_height(by, 1 << (bhl & 7))
        need = dll.level_size(row, by, bh)
        src = bytes(buf[level0:level0 + need])
        if len(src) < need:
            continue
        dst = ctypes.create_string_buffer(row * by)
        dll.untile(src, dst, row, by, bh)
        yield w, h, nvn, dxgi, dst.raw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("--packs", help="comma separated pack names (default: all)")
    ap.add_argument("--out", help="default <data>/textures/dump")
    ap.add_argument("--list", help="also write '<hash> <pack> <file>' per texture seen (uhd_pack.py --sources)")
    a = ap.parse_args()
    listing = open(a.list, "w", encoding="utf-8") if a.list else None
    out = a.out or os.path.join(a.data, "textures", "dump")
    os.makedirs(out, exist_ok=True)
    have = {n.rsplit("_", 1)[-1] for n in os.listdir(out) if n.endswith(".dds")}
    off = set()
    ini = os.path.join(a.data, "build", "settings.ini")
    if os.path.exists(ini):
        for line in open(ini, encoding="utf-8", errors="replace"):
            k, _, v = line.partition("=")
            if k.strip() == "mods_disabled":
                off = {x.strip() for x in v.split(",") if x.strip()}
    if "NX_MODS_DISABLED" in os.environ:  # environment overrides the ini, as in tms.exe
        off = {x.strip() for x in os.environ["NX_MODS_DISABLED"].split(",") if x.strip()}
    mods_dir = os.path.join(a.data, "mods")
    mods = sorted(m for m in os.listdir(mods_dir) if m not in off) if os.path.isdir(mods_dir) else []
    pack_dir = os.path.join(a.data, "romfs", "Pack")
    names = a.packs.split(",") if a.packs else sorted(n[:-4] for n in os.listdir(pack_dir) if n.endswith(".cpk"))
    total = new = 0
    for name in names:
        path = os.path.join(pack_dir, name + ".cpk")
        for m in mods:
            mp = os.path.join(mods_dir, m, "romfs", "Pack", name + ".cpk")
            if os.path.exists(mp):
                path = mp
                break
        c = Cpk(path)
        n0 = new
        for r in c.files:
            p = c.path(r)
            if p.endswith((".usm", ".acb", ".awb", ".lua", ".msbt", ".anm", ".lig")):
                continue
            buf = read_entry(c, r)
            at = buf.find(b"BNTX")
            while at >= 0:
                try:
                    for w, h, nvn, dxgi, lin in bntx_textures(buf, at):
                        total += 1
                        hsh = "%016x.dds" % dll.tex_hash(lin, len(lin))
                        if listing:
                            listing.write("%s %s %s\n" % (hsh[:16], name, p))
                        if hsh in have:
                            continue
                        have.add(hsh)
                        new += 1
                        with open(os.path.join(out, "%ux%u_%02x_%s" % (w, h, nvn, hsh)), "wb") as f:
                            f.write(dds(w, h, dxgi, lin))
                except (struct.error, IndexError):
                    pass
                at = buf.find(b"BNTX", at + 4)
        print("%s: %d new textures (%s)" % (name, new - n0, os.path.relpath(path, a.data)), flush=True)
    print("%d textures seen, %d new in %s" % (total, new, out))


if __name__ == "__main__":
    main()
