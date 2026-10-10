# Rusty (PC-98), native recompilation

*Rusty* (C-Lab, 1993, NEC PC-98) as a native program for Linux and Windows. The game code is translated from x86 to
C; a small runtime emulates the PC-98 hardware around it.

This repository and the releases contain no game data and no ROMs. You need your own copy of the game.

## What you need

- The game, one of:
  - the Japanese original,
  - the English translation by 46 OkuMen (v1.0.3), also with the "Add by Flurror" image.

  As a hard disk image (`.hdi`), floppy images (`.d88`, `.hdm`) or a folder with the game files.
- `FONT.ROM` (288768 bytes): the PC-98 kanji font ROM. Without it, all text is empty.
- `ym2608_adpcm_rom.bin` (8192 bytes): the YM2608 rhythm ROM. Without it, the drums are silent.

## Launch

Download the package for your system from the releases and unpack it. Put the game image and the two ROMs in the
same folder as the program (the ROMs can also go in a folder `roms` there).

- Windows: double-click `rusty.exe`, or drag the disk image onto it.
- Linux: `./rusty`, or `./rusty <game>`.

Without `<game>`, the program starts the game in its own folder. The log is in `rusty.log` next to the program
(Windows) or on the terminal (Linux).

## Controls

| Key | Action |
|---|---|
| Arrow keys | move |
| Up + Left/Right | run |
| Z | jump |
| X or Space | whip (hold to hang from hooks) |
| Z + X | special weapon (stand still) |
| Esc | pause |
| Return | skip the opening, select in menus |
| F1 / F10 | game speed up / down |
| F5 / F9 | save / load the current slot |
| F6 / F7 | previous / next save slot (1-9) |
| F11 | full screen |
| G | god mode |

Gamepads work too (Xbox names): D-pad or left stick move, A jump, X whip, B + left/right run, Y special weapon,
Start = Return, Back = pause, RB / LB game speed.

Saves work during play, not in menus or in the pause. They go to `save/` next to the program.

## Build

Linux, with CMake 3.25+, Ninja, a C/C++ compiler, [uv](https://docs.astral.sh/uv/), SDL3 and `glslc` (shaderc).
The build reads your game files and generates the C code from them.

1. Put the disk images in `roms/`:
   - the Japanese original: `roms/Rusty (19xx)(C-Lab)/Rusty (19xx)(C-Lab).hdi` (required),
   - the English translation: `roms/Rusty (Japan) [T-En by 46 OkuMen v1.0.3].hdi` (optional).
2. Unpack the game files:

   ```sh
   uv sync
   python3 tools/extract.py "roms/Rusty (19xx)(C-Lab)/Rusty (19xx)(C-Lab).hdi" -o build/files/ja
   python3 tools/lz_unpack.py build/files/ja/RUSTY build/unpacked/ja --copy-plain
   python3 tools/exepack.py build/unpacked/ja

   python3 tools/extract.py "roms/Rusty (Japan) [T-En by 46 OkuMen v1.0.3].hdi" -o build/files/base
   python3 tools/lz_unpack.py build/files/base build/unpacked/base --copy-plain
   python3 tools/exepack.py build/unpacked/base
   ```

3. Build:

   ```sh
   cmake --preset linux-dev && cmake --build --preset linux-dev
   build/linux-dev/rusty "roms/Rusty (Japan) [T-En by 46 OkuMen v1.0.3].hdi"
   ```

Release packages (in `build/packages/`):

```sh
cmake --preset linux-release && cmake --build --preset linux-release && cpack --preset linux-release
cmake --preset win-release && cmake --build --preset win-release && cpack --preset win-release
```

The Windows package is cross-compiled with MinGW-w64. It needs the SDL3 MinGW development package
(`SDL3-devel-<version>-mingw.tar.gz` from the SDL releases) unpacked in `tools/external/`.

Tests: `uv run pytest` (needs `nasm`).

## License

The code of this project: MIT License (`LICENSE`). Third-party code: ymfm (BSD-3-Clause, `third_party/ymfm`),
SDL3 (zlib). The game and the English translation belong to their owners. The release programs contain code
translated from the game; the MIT License does not cover that code.
