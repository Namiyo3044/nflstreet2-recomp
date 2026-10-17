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
```

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

## Licence note

The port links DolRecomp's CPU runtime (`src/cpu/cpu.c`), which is GPLv3, so the port as a
whole is GPLv3: if you distribute builds, you must also publish the source.
