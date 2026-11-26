#!/bin/bash
# package-linux.sh: the Linux release archive, TMSRecomp-<version>-linux.tar.gz, from this checkout (the Linux
# counterpart of package.ps1; .github/workflows/linux.yml runs it on every version tag).
#   tms + launcher (RPATH $ORIGIN/lib, SDL3 and libvpx in lib/), the self-contained shader translator (shaderc/),
#   setup/ (nstool, sdk/guest.h, the setup tools, fasttex.so), README.txt, LICENSE, THIRD_PARTY.txt.
#   NO game code or data: game.so, the lifted C, translated shaders and caches are made on the player's PC by setup.
# Needs: clang, lld, cmake, ninja, pkg-config, the dev packages of sdl3 vulkan vpx cairo pangocairo, dotnet-sdk-10.0,
# python3; the Ryujinx (Ryubing) source with tools/shaderc/patches applied (RYU_DIR) and a built nstool 1.9.2 (NSTOOL_DIR).
# Usage: RYU_DIR=<ryubing> NSTOOL_DIR=<nstool> [OUT=<dir>] [TMS_VERSION=v0.1.4] bash tools/package/package-linux.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RYU_DIR="$(realpath "${RYU_DIR:-$ROOT/private/third_party/ryubing}")"
NSTOOL_DIR="$(realpath "${NSTOOL_DIR:-$ROOT/private/third_party/nstool}")"
OUT="$(mkdir -p "${OUT:-$ROOT/private/dist}" && realpath "${OUT:-$ROOT/private/dist}")"
TMS_VERSION="${TMS_VERSION:-$(git -C "$ROOT" describe --tags --abbrev=0 2>/dev/null || echo dev)}"
TMS_VERSION="${TMS_VERSION%-beta}"
B="$ROOT/private/linux-release"; ST="$OUT/stage/TMSRecomp"
for f in "$RYU_DIR/LICENSE.txt" "$NSTOOL_DIR/bin/nstool" "$NSTOOL_DIR/LICENSE"; do [ -e "$f" ] || { echo "missing $f (see the header)"; exit 1; }; done

# program: its own build folder without lifted code (no game library), libraries from lib/ next to the programs
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DCMAKE_BUILD_RPATH=$ORIGIN/lib' -DNX_GEN="$B/no-gen" >/dev/null
ninja -C "$B" tms launcher
# shader translator: self-contained (players need no .NET install)
rm -rf "$B/shaderc"
dotnet publish "$ROOT/tools/shaderc/NxShader.csproj" -c Release -r linux-x64 --self-contained true -o "$B/shaderc" \
  -p:RyuDir="$RYU_DIR" -p:DebugType=none -p:SatelliteResourceLanguages=en

rm -rf "$OUT/stage"; mkdir -p "$ST/lib" "$ST/setup/sdk" "$ST/setup/tools/"{recomp,texextract,shaderscan,setup}
cp -p "$B/tms" "$B/launcher" "$ROOT/data/subtitles_en.tsv" "$ST/"; strip -s "$ST/tms" "$ST/launcher"
cp -rp "$ROOT/data/resist_icons" "$ST/"
for l in libSDL3.so libvpx.so; do  # exactly the files the program was linked against
  f="$(ldd "$B/tms" | awk -v l="$l" 'index($1, l) == 1 {print $3; exit}')"
  [ -n "$f" ] && [ -e "$f" ] || { echo "$l not found by ldd"; exit 1; }
  cp -pL "$f" "$ST/lib/$(basename "$f")"
done
cp -a "$B/shaderc" "$ST/shaderc"; find "$ST/shaderc" -name '*.pdb' -delete
cp -p "$NSTOOL_DIR/bin/nstool" "$ST/setup/"
cp -p "$ROOT/runtime/include/guest.h" "$ST/setup/sdk/"
cp -p "$ROOT"/tools/recomp/{link.py,lift.py,a32.py,vfp.py,names.txt} "$ST/setup/tools/recomp/"
cp -p "$ROOT"/tools/{nso.py,cpk.py,vcdiff.py} "$ST/setup/tools/"
clang -O2 -shared -fPIC -o "$ST/setup/tools/texextract/fasttex.so" "$ROOT/tools/texextract/fasttex.c"
cp -p "$ROOT/tools/texextract/texextract.py" "$ST/setup/tools/texextract/"
cp -p "$ROOT/tools/shaderscan/shaderscan.py" "$ST/setup/tools/shaderscan/"
cp -p "$ROOT"/tools/setup/{setup.py,build_game.py} "$ST/setup/tools/setup/"
cp -p "$ROOT/LICENSE" "$ST/"; cp -p "$ROOT/tools/package/README-linux.txt" "$ST/README.txt"
# no game code or data in the archive (same rule as package.ps1)
bad=$(find "$ST" -type f \( -regextype posix-extended -regex '.*/(game\.so|game\.dll|image\.bin|layout\.json|shaders\.bin|pipelines\.(bin|recipes)|files\.txt|funcs\.h|table\.c|f_[0-9]+\.c|icon\.(jpg|ico)|launcher_hero\.bmp)' \
      -o -name '*.nca' -o -name '*.nsp' -o -name '*.o' -o -name '*.cpk' -o -name '*.keys' \))
[ -z "$bad" ] || { echo "game code/data staged, refusing: $bad"; exit 1; }
[ "$(stat -c %s "$ST/tms")" -lt 8388608 ] || { echo "tms is too big: does it contain game code?"; exit 1; }
notice() { printf '==== %s\n\n' "$1"; if [ -f "$2" ]; then cat "$2"; else echo "(see $2)"; fi; printf '\n\n'; }
{ printf 'Third-party software in this package\n\n'
  notice "SDL3 (lib/libSDL3.so.0), zlib licence" /usr/share/doc/libsdl3-0/copyright
  notice "libvpx (lib/libvpx.so.*, VP9 movies), BSD-3-Clause" "$(ls /usr/share/doc/libvpx[0-9]*/copyright 2>/dev/null | head -1)"
  notice "Ryujinx shader translator (shaderc/) and BC7 tables (in tms, runtime/src/bc7.h), MIT" "$RYU_DIR/LICENSE.txt"
  notice "Spv.Generator (shaderc/, sharpening/overlay shaders in tms), MIT" "$RYU_DIR/src/Spv.Generator/LICENSE"
  notice "nstool (setup/nstool), MIT" "$NSTOOL_DIR/LICENSE"
  notice ".NET runtime (shaderc/, self-contained), MIT" "$(ls /usr/lib/dotnet/LICENSE.txt /usr/share/dotnet/LICENSE.txt 2>/dev/null | head -1)"
} > "$ST/THIRD_PARTY.txt"
out="$OUT/TMSRecomp-$TMS_VERSION-linux.tar.gz"
tar -czf "$out.part" -C "$OUT/stage" --owner=0 --group=0 TMSRecomp && mv "$out.part" "$out"
echo "wrote $out ($(du -m "$out" | cut -f1) MB)"
