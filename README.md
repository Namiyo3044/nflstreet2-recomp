# NFL Street 2 native PC port

An unofficial native Windows port of **NFL Street 2** (GameCube, USA, GN7E69 rev 0),
built by statically recompiling the game's own `main.dol` with
[DolRecomp](https://github.com/expansionpak/dolrecomp) and running it on a custom runtime.

**This repository contains no game code or data.** You need your own copy of the game.
Everything derived from the disc (the DOL, extracted files, and the recompiler's output)
lives outside this repo and is ignored by `.gitignore`.

## What you need

- **Your own NFL Street 2 for GameCube, USA (GN7E69), first release (rev 0)**, as a disc image
  (`.iso` / `.gcm`) or a folder from Dolphin's *Extract Entire Disc*. Compressed `.rvz` / `.ciso`
  images: convert them to `.iso` in Dolphin first (right-click the game, *Convert File*).
- **Windows 10 or 11**, 64-bit.
- **Build Tools for Visual Studio 2022** (the launcher installs it for you; or free, from
  [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/)), with the
  *Desktop development with C++* workload and, in its options, **C++ Clang tools for Windows**.
  (A full Visual Studio 2022 with the same pieces works too, as does a separate
  [LLVM](https://releases.llvm.org/) install on the PATH in place of the Clang option.)
- About 4 GB of disk space and an internet connection for the first setup.

## Setup (once)

Double-click **`Launcher.bat`**. Its window takes you through it:

1. **Install Build Tools**, if they aren't there yet (Microsoft's free C++ compiler, which builds
   the port on your PC; about 6 GB, installed for you with winget).
2. **Browse...** for your disc image (`.iso`, `.gcm` or `.nkit.iso`). Compressed `.rvz` / `.ciso`
   images need converting to `.iso` in Dolphin first (right-click the game, Convert File).
3. **Set Up**. It unpacks the disc into `C:\nfls2\disc`, checks it's the right version, downloads
   and builds [DolRecomp](https://github.com/expansionpak/dolrecomp), recompiles the game's code
   from your own `main.dol` into `C:\nfls2\recomp`, and builds the port with an icon made from your
   disc's own banner. The first set up takes several minutes. It also puts an
   **NFL Street 2 Recomp** shortcut on the desktop (untick the box to skip it).
4. **Play**.

The same set up without the window, in PowerShell in this folder (`setup.bat` also works):

```powershell
.\setup.ps1 -Disc "D:\Games\NFL Street 2.iso"
```

Running it again only does what's missing. `-Root D:\nfls2` puts all of it somewhere else,
`-NoShortcut` skips the shortcut.

## Play

Double-click the desktop shortcut, **Play** in the launcher or **`play.bat`**, or in PowerShell:

```powershell
.\play.ps1
```

Rebuilds if anything changed and opens the game. Esc quits; **F1** (or the controller's
Back / View button) opens the port's menu: video, controls, sound and the recomp settings.
A controller works out of the box; the keyboard keys are listed at the top of `play.ps1` and
can be changed in the menu. Settings are saved in `C:\nfls2\settings.ini`, the memory card in
`C:\nfls2\saves`.

Optional extras:

- **Custom textures** (Dolphin's format, `.png` / `.dds`): put the pack's files in
  `C:\nfls2\textures`.
- **Text changes**: `text-replacements.txt`; **menu colors**: `ui-colors.txt` (both explained
  inside the files).
- **Replacement movies**: files in `C:\nfls2\replace` are read instead of the disc's.

`play.ps1` lists all its switches at the top; the log of each run goes to `play.txt`.

## Credits

**NFL Street 2** is © Electronic Arts / EA SPORTS BIG. This is an unofficial fan project, not
affiliated with or endorsed by EA, the NFL or Nintendo.

**The NFL Street 2 decomp** ([mitsevox/nflstreet2](https://github.com/mitsevox/nflstreet2)),
by the team behind NFL Street Reborn. Their mapping of the game's functions is what the
recompiler is given to know where each function starts and ends: `data\gn7e69-functions.map`
lists those addresses and sizes (no names), taken from their symbol map. This repo contains none of
the decomp's source code, and the two are
separate projects: the decomp is rebuilding the game as readable C, while this port recompiles
the original program as it is. Thanks for all the work understanding this game.

**[DolRecomp](https://github.com/expansionpak/dolrecomp)**: the static recompiler and the CPU
runtime this port is built on (GPLv3).

Libraries used (SDL is downloaded at build time; the rest are in `src/third_party`):

- [SDL 3](https://github.com/libsdl-org/SDL): window, input and sound (zlib license)
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski: replacement movie
  playback (MIT)
- [stb_image, stb_image_write, stb_truetype](https://github.com/nothings/stb) by Sean Barrett:
  textures, screenshots and menu text (public domain / MIT)
- [bcdec](https://github.com/iOrange/bcdec) by Sergii "iOrange" Kudlai: compressed texture
  decoding (MIT / Unlicense)
- [Anton](https://github.com/googlefonts/AntonFont) font by The Anton Project Authors: the
  port's own menus (SIL Open Font License 1.1, see `src/third_party/Anton-OFL.txt`)

## License note

The port links DolRecomp's CPU runtime (`src/cpu/cpu.c`), which is GPLv3, so the port as a
whole is GPLv3: if you distribute builds, you must also publish the source.
