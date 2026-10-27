# NFL Street 2 native PC port

An unofficial native Windows port of **NFL Street 2** (GameCube, USA, GN7E69 rev 0),
built by statically recompiling the game's own `main.dol` with
[DolRecomp](https://github.com/expansionpak/dolrecomp) and running it on a custom runtime.

**This repository contains no game code or data.** You need your own copy of the game.
Everything derived from the disc (the DOL, extracted files, and the recompiler's output)
lives outside this repo and is ignored by `.gitignore`.

## Expected layout

```
C:\nfls2\
  disc\            your extracted disc (sys\main.dol, files\...)
  decomp\          https://github.com/mitsevox/nflstreet2 (symbol source)
  symbols\         main.map from tools\export_symbols.py
  recomp\dolrecomp\generated\   DolRecomp output for your main.dol
  tools\dolrecomp\ DolRecomp checkout
  port\            this repo
  ps2\             optional: .DAT files from your own PS2 copy (see below)
```

### PS2 online menus (optional)

The GameCube version has no online mode. If you copy these files from your own
PS2 disc of the game into `C:\nfls2\ps2`, the main menu's **Online** shows the
PS2 version's online menus, run by a small player for EA's menu format
(`src/apt`): `LGENERAL.DAT`, `SGENERAL.DAT`, `FONLINE.DAT`, `FONTS.DAT` and every
`SON*.DAT`. Without them, Online shows the port's own page.

## Build (Windows, clang-cl + Ninja)

From **Developer PowerShell for VS**:

```powershell
cd C:\nfls2\port
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Paths to DolRecomp and the generated code default to the layout above; override with
`-DNFLS2_DOLRECOMP_DIR=...` and `-DNFLS2_GENERATED_DIR=...`.

## Run

```powershell
.\build\nfls2.exe C:\nfls2\disc --map C:\nfls2\symbols\main.map
```

Options: `--max-steps N` (default 50,000,000), `--quiet` (don't list functions as they are
first entered), `--skip-hash` (run a DOL that isn't the verified build).

## Credits

**NFL Street 2** is © Electronic Arts / EA SPORTS BIG. This is an unofficial fan project, not
affiliated with or endorsed by EA, the NFL or Nintendo.

**The NFL Street 2 decomp** ([mitsevox/nflstreet2](https://github.com/mitsevox/nflstreet2)),
by the team behind NFL Street Reborn. Their mapping of the game's functions is what the
recompiler is given to know where each function starts and ends (see `tools\export_symbols.py`
in the layout above). This repo contains none of the decomp's source code, and the two are
separate projects: the decomp is rebuilding the game as readable C, while this port recompiles
the original program as it is. Thanks for all the work understanding this game.

**[DolRecomp](https://github.com/expansionpak/dolrecomp)**: the static recompiler and the CPU
runtime this port is built on (GPLv3).

Libraries used (SDL is downloaded at build time; the rest are in `src/third_party`):

- [SDL 3](https://github.com/libsdl-org/SDL): window, input and sound (zlib licence)
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski: replacement movie
  playback (MIT)
- [stb_image, stb_image_write, stb_truetype](https://github.com/nothings/stb) by Sean Barrett:
  textures, screenshots and menu text (public domain / MIT)
- [bcdec](https://github.com/iOrange/bcdec) by Sergii "iOrange" Kudlai: compressed texture
  decoding (MIT / Unlicense)
- [Anton](https://github.com/googlefonts/AntonFont) font by The Anton Project Authors: the
  port's own menus (SIL Open Font License 1.1, see `src/third_party/Anton-OFL.txt`)

## Licence note

The port links DolRecomp's CPU runtime (`src/cpu/cpu.c`), which is GPLv3, so the port as a
whole is GPLv3: if you distribute builds, you must also publish the source.
