"""Offline shader translation: every Maxwell shader in the game's CPK packs -> <data>/cache/shaders.bin, keyed exactly
as tms.exe does (vk_shader.cpp translate()), so the game does 0 shader translations in areas nobody has visited.
Same idea as tools/texextract. The release ships only this tool; the translations are made on the user's PC.

    python tools/shaderscan/shaderscan.py <data> [--packs pack_030_etc] [--dll PATH] [--cache FILE] [--check]

Shaders are BNSH blobs (Shaders/*.pso/.vso, APAK packs .pspk/.vspk/.cspk, inside other files such as effects, and in
the game executables <data>/link/image.bin):
every "BNSH" found in a file is scanned. A stage record in a BNSH is: control offset u64, code offset u64, then two
u32 sizes (offsets from the BNSH start). Graphics code starts with the 0x30-byte NVN header.

The translator is the same sidecar tms.exe runs (dotnet NxShader.dll); its file size + time stamp the cache, so use
the SAME dll as the game (default: <data>/build/shaderc, else <data>/shaderc, like tms.exe). Only appends: entries
already in the cache are skipped, an existing cache from another translator build is left alone (s30 rule).
Do not run while tms.exe runs (both append to the file).
Vertex shaders are translated with float inputs; integer-attribute variants (an attribute with a SINT / UINT format)
are keyed by the attribute types too. Every integer variant seen in play (s95: 12 of them, character / shading_effect /
stainless .vspk and image.bin) is attribute 7 = uint (skinning indices), so the vertex shaders of image.bin and
pack_030_etc (Shaders/*.vspk) also get that variant (~300 more translations).
--check: translate nothing, report how many entries of the existing cache this scan reproduces (key check).
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "texextract"))
from cpk import Cpk  # noqa: E402
from texextract import read_entry  # noqa: E402

CODE_MAGIC, CTRL_MAGIC, LEN = 0x12345678, 0x98761234, 0x20000
ZERO32 = bytes(32)


def fnv(b, h=0xcbf29ce484222325):
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def bnsh_stages(buf, at):
    """(control bytes, code bytes) per stage record of the BNSH at buf[at]."""
    end = buf.find(b"BNSH", at + 4)
    end = len(buf) if end < 0 else end
    ctl = buf.find(struct.pack("<I", CTRL_MAGIC), at, end)
    while ctl >= 0:
        ref = buf.find(struct.pack("<Q", ctl - at), at, end)
        if ref >= 0 and ref + 24 <= end:
            # the game reads control +0x6fc..+0x750, past the stated sizes, and code until the translator stops:
            # slice both to the BNSH end, as they lie in guest memory (code_size from the translator bounds the hash)
            code_off = struct.unpack_from("<Q", buf, ref + 8)[0]
            if at + code_off < end:
                yield buf[ctl:end].ljust(0x800, bytes(1)), buf[at + code_off:end]
        ctl = buf.find(struct.pack("<I", CTRL_MAGIC), ctl + 4, end)


def shader_key(ctl, code):
    """Like vk_shader.cpp program_of(): (stage, local, consts_off, consts_size, code after the NVN header)."""
    header = u32(code, 0) == CODE_MAGIC
    sph = u32(code, 0x30) if header else 0
    sph_type, shader_type = sph & 0x1F, (sph >> 10) & 0xF
    stage = shader_type if sph_type == 1 and 1 <= shader_type <= 4 else 5 if sph_type == 2 else 0
    off = 0x30 if header else 0
    local = [1, 1, 1]
    if stage == 0 and u32(ctl, 0) == CTRL_MAGIC:
        local = [max(1, u32(ctl, 0x748 + 4 * k)) for k in range(3)]
    csize, coff = u32(ctl, 0x6FC), u32(ctl, 0x700)
    return stage, local, coff - off if csize else 0, csize, code[off:]


def keys(stage, local, coff, csize, code, types=ZERO32):
    # types: vertex attribute types (0 float, 1 sint, 2 uint per slot; all-float = the base module); others ignore it
    inp = struct.pack("<4I", stage, coff, csize, local[0] | local[1] << 10 | local[2] << 20 if stage == 0 else 0)
    prefix = fnv(code[:256], fnv(types, fnv(inp)))

    def full(size):
        h = fnv(code[:size])
        return fnv(code[coff:coff + csize], h) if csize and coff + csize <= LEN else h
    return prefix, full


class Sidecar:
    def __init__(self, dll):
        sc = os.path.join(os.path.dirname(dll), "hostfxr.dll")  # self-contained build (release ZIP): no .NET install
        cmd = [os.path.abspath(dll[:-4] + ".exe")] if os.path.exists(sc) else ["dotnet", dll]
        if sys.platform.startswith("linux"):  # self-contained linux-x64 publish: NxShader + libhostfxr.so
            sc = os.path.join(os.path.dirname(dll), "libhostfxr.so")
            cmd = [os.path.abspath(dll[:-4])] if os.path.exists(sc) else ["dotnet", dll]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def rd(self, n):
        b = self.p.stdout.read(n)
        if len(b) != n:
            raise RuntimeError("shader translator: read failed (did it crash?)")
        return b

    def translate(self, stage, local, coff, csize, code, types=ZERO32):
        """Same protocol as vk_shader.cpp translate_sidecar(); returns (code_size, spirv, res, frag_outputs) or None."""
        w = self.p.stdin
        w.write(struct.pack("<3I", 0x4853584E, stage, LEN))
        if stage == 0:
            w.write(struct.pack("<3I", *local))
        w.write(struct.pack("<2I", coff, csize))
        if stage == 1:
            w.write(types)
        w.write(code[:LEN].ljust(LEN, b"\0"))
        w.flush()
        if u32(self.rd(4), 0) != 0:
            return None
        code_size, n = struct.unpack("<2I", self.rd(8))
        spirv = self.rd(n)
        nres = u32(self.rd(4), 0)
        res = self.rd(20 * nres)  # Resource: u8 kind, u8 set, u16 binding, i32 slot, handle, type, count
        return code_size, spirv, nres, res, self.rd(4)


def read_cache(path):
    """(stamp, {prefix: [(full, code_size)]}) of an existing shaders.bin, or (None, {})."""
    if not os.path.exists(path):
        return None, {}
    b = open(path, "rb").read()
    stamp, have, o = struct.unpack_from("<3Q", b, 0), {}, 24
    while o + 24 <= len(b):
        prefix, full, code_size, n = struct.unpack_from("<QQII", b, o)
        o += 24 + 4 * n
        nres = u32(b, o)
        o += 4 + 20 * nres + 4
        have.setdefault(prefix, []).append((full, code_size))
    return stamp, have


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("--packs", help="comma separated pack names (default: all)")
    ap.add_argument("--dll", help="NxShader.dll (default: as tms.exe finds it)")
    ap.add_argument("--cache", help="default <data>/cache/shaders.bin")
    ap.add_argument("--check", action="store_true", help="only count existing cache entries this scan reproduces")
    ap.add_argument("--set-aside", action="store_true",
                    help="a cache from another translator build is renamed to shaders.bin.<time>.old (setup)")
    a = ap.parse_args()
    dll = a.dll or next((p for p in (os.path.join(a.data, "build", "shaderc", "NxShader.dll"),
                                     os.path.join(a.data, "shaderc", "NxShader.dll")) if os.path.exists(p)), None)
    if not dll:
        sys.exit("NxShader.dll not found (pass --dll)")
    st = os.stat(dll)
    stamp = (0x31435358, st.st_size, st.st_mtime_ns // 100 + 116444736000000000)
    cache = a.cache or os.path.join(a.data, "cache", "shaders.bin")
    old, have = read_cache(cache)
    if old and (old[0] != stamp[0] or old[1] != stamp[1] or old[2] // 10**7 != stamp[2] // 10**7) and a.set_aside:
        aside = f"{cache}.{int(time.time())}.old"  # kept, never deleted (the game would not use it anyway)
        os.replace(cache, aside)
        print(f"{cache} was made by another translator build: moved to {aside}")
        old, have = None, {}
    if old and (old[0] != stamp[0] or old[1] != stamp[1] or old[2] // 10**7 != stamp[2] // 10**7):
        sys.exit(f"{cache} was made by another translator build: left alone (use the game's own NxShader.dll)")
    print(f"translator {dll}; cache {cache}: {sum(map(len, have.values()))} entries")

    pack_dir = os.path.join(a.data, "romfs", "Pack")
    names = a.packs.split(",") if a.packs else sorted(n[:-4] for n in os.listdir(pack_dir) if n.endswith(".cpk"))
    seen, found, matched, new, failed, short = set(), 0, 0, 0, 0, 0
    uint7 = bytes(7) + b"\x02" + bytes(24)  # attribute 7 = uint
    sc, out = None, None

    def buffers():
        # the game executables embed shaders too (boot, movie, system UI: 169 BNSH in image.bin, missed by packs only)
        image = os.path.join(a.data, "link", "image.bin")
        if not a.packs and os.path.exists(image):
            with open(image, "rb") as f:
                yield "link/image.bin", [f.read()]
        for name in names:  # shaders are not modded (no mod replaces pack_030_etc); base packs only
            c = Cpk(os.path.join(pack_dir, name + ".cpk"))
            yield name, (read_entry(c, r) for r in c.files
                         if not c.path(r).endswith((".usm", ".acb", ".awb", ".lua", ".msbt", ".anm", ".lig", ".gtx")))

    for name, bufs in buffers():
        for buf in bufs:
            at = buf.find(b"BNSH")
            while at >= 0:
                for ctl, code in bnsh_stages(buf, at):
                    stage, local, coff, csize, body = shader_key(ctl, code)
                    if len(body) < 256:  # the prefix hash reads 256 bytes: the rest would be other guest memory
                        short += 1
                        continue
                    skinned = stage == 1 and name in ("link/image.bin", "pack_030_etc")
                    for types in (ZERO32, uint7) if skinned else (ZERO32,):
                        prefix, full = keys(stage, local, coff, csize, body, types)
                        if (prefix, fnv(body)) in seen:
                            continue
                        seen.add((prefix, fnv(body)))
                        found += 1
                        if any(s <= len(body) and full(s) == h for h, s in have.get(prefix, ())):  # as translate()
                            matched += 1
                            continue
                        if a.check:
                            continue
                        sc = sc or Sidecar(dll)
                        t = sc.translate(stage, local, coff, csize, body, types)
                        if not t or not t[0] or t[0] > len(body):
                            failed += 1
                            continue
                        code_size, spirv, nres, res, frag = t
                        if out is None:
                            os.makedirs(os.path.dirname(cache), exist_ok=True)
                            out = open(cache, "ab")
                            if old is None:
                                out.write(struct.pack("<3Q", *stamp))
                        out.write(struct.pack("<QQII", prefix, full(code_size), code_size, len(spirv) // 4) + spirv +
                                  struct.pack("<I", nres) + res + frag)
                        out.flush()
                        have.setdefault(prefix, []).append((full(code_size), code_size))
                        new += 1
                at = buf.find(b"BNSH", at + 4)
        print(f"{name}: {found} shaders so far, {matched} already cached, {new} translated, {failed} failed")
    print(f"done: {found} shaders, {matched} already cached, {new} translated, {failed} failed, "
          f"{short} too short to key")


if __name__ == "__main__":
    main()
