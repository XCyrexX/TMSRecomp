TMSRecomp - Tokyo Mirage Sessions #FE Encore, recompiled (Linux x86-64 build)
==============================================================================

This archive holds the program and its setup tools only: no game code, no game files. On the first start the
launcher's Setup page builds the game from YOUR OWN copy (Switch, title 0100A9400C9C2000, base game 1.0.0 without
update): pick your NSP and the prod.keys from your own console (or a folder you already extracted with exefs/ and
romfs/). Setup copies the game files (about 13 GB), translates the game code, compiles it into game.so next to tms and
translates all shaders. It takes about 5 to 15 minutes and needs no internet.

Experimental: tested on Ubuntu 26.04 with an NVIDIA GTX 1650 and an Intel UHD 770 (setup, story scenes, field, camp
menu, saving and loading, F1 menu, 60 fps), and on a ROG Ally (Z1 Extreme, Bazzite, Mesa RADV): 60 fps on Balanced,
battles included; a few heavy scenes around 50. Other AMD GPUs and the Steam Deck should behave like the Ally.

Requirements
- Linux x86-64 with glibc 2.43 or newer (for example Ubuntu 26.04, Fedora 44, Arch, Bazzite), a GPU with Vulkan 1.3
- For the setup: python3 with the capstone module, clang and lld. For example:
    Ubuntu / Debian:  sudo apt install python3-capstone clang lld
    Fedora:           sudo dnf install python3-capstone clang lld
    Arch:             sudo pacman -S python-capstone clang lld
    Immutable systems (Bazzite, SteamOS): pip install --user capstone, and clang + lld from a toolbox / distrobox
- cairo and pango (installed on every desktop system). SDL3 and libvpx come with this build (lib/).
- About 15 GB free disk space for the game files

Start
1. Extract this folder anywhere (keep the setup/ folder).
2. Run ./launcher. The Setup page opens: pick your game, press "Set up game files", wait for "Done".
3. PLAY. The game's saves, caches, logs and screenshots live in the game files folder.

Files
- tms            the game (F1 or L3+R3: settings overlay; F12: screenshot; Alt+Enter / F11: window)
  command line: --data <folder> (game files), --set key=value (any settings.ini key, this run only, repeatable)
- launcher       settings + PLAY
- settings.ini   written on the first start, next to tms (or ~/.config/TMSRecomp/ if this folder is read-only)
- subtitles_en.tsv   English subtitles for the battle voices
- shaderc/       shader translator (Ryujinx shader compiler, MIT), carries its own .NET runtime
- lib/           SDL3 and libvpx
- setup/         first-run setup: nstool, our build tools (uses python3, clang and lld from your system)
- game.so        made by setup from your game (not in the archive)

Differences from Windows: no AMD FSR 3 and no NVIDIA DLSS (they fall back to SMAA). FXAA, SMAA, MSAA, FSR 1 and
sharpening work. Mods, UHD textures, 4K movies and custom battle music work as described in the Windows README.

Licences: TMSRecomp is GPL-2.0-or-later (LICENSE). Third-party notices: THIRD_PARTY.txt.

TMSRecomp is an unofficial fan project. It is not affiliated with, endorsed or sponsored by Nintendo, ATLUS, SEGA or
Intelligent Systems. Tokyo Mirage Sessions #FE Encore and all related names, characters and assets are trademarks and
copyrights of their owners. You need your own legally obtained copy of the game; this project does not help you obtain
the game or its keys, and none of its files may be shared with these programs.
