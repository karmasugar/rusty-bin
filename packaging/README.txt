Rusty (PC-98) - native recompilation
====================================

This program runs the PC-98 game "Rusty" (C-Lab, 1993) as native code. The game code was translated from x86 to C.
The package does not contain the game data or any ROM. You must use your own copy of the game.

Supported versions
------------------
- The Japanese original.
- The English translation by 46 OkuMen (v1.0.3), also with the "Add by Flurror" image.
Other versions stop with a message that names the unknown file.

Start
-----
Linux:      ./rusty <game>
Windows:    rusty.exe <game>   (or drag the disk image onto rusty.exe)

Without <game> (for example a double-click on rusty.exe), the program starts the game in its own folder:
the one .hdi file there, or the floppy images there, or the game files (BOOT.COM) there.

<game> can be:
- a hard disk image (.hdi),
- a floppy image (.d88 or .hdm), or a folder with all floppy images of the game,
- a folder with the game files (BOOT.COM and the other files).

Example:
    ./rusty "Rusty (Japan) [T-En by 46 OkuMen v1.0.3].hdi"
On Windows the program has no console window: its log is in rusty.log next to rusty.exe.

ROMs
----
Put these files next to the game image, in a folder "roms" next to this program, or next to this program:
- FONT.ROM (288768 bytes): the PC-98 kanji font ROM. Without it, all text is empty.
- ym2608_adpcm_rom.bin (8192 bytes): the YM2608 rhythm ROM. Without it, the drums are silent.
When a ROM is missing, the game shows a message on the screen, and the log lists the folders it searched.
You can also set RUSTY_ROM_DIR=<folder>, RUSTY_FONT_ROM=<file> or RUSTY_RHYTHM_ROM=<file>.

Keys
----
Arrow keys (or keypad 8, 2, 4, 6), Z, X, Space, Return, Esc, F1, F2 and F10 are the PC-98 keys of the same name.
In the game:
Arrow keys      move
Up + Left/Right run
Z               jump
X (or Space)    whip (hold to hang from hooks)
Z + X           special weapon (stand still, press both keys at the same time)
Esc             pause
F1 / F10        game speed up / down
Return          skip the opening, select in menus
F5              save the game in the current slot (slots 1-9)
F9              load the current slot
F6 / F7         previous / next slot (the screen shows the stage and the time of the save)
F11             full screen
G               god mode (debug cheat)

Saving and loading work during play (not in the title menu, other menus or the pause). To continue a saved game,
start a game, then select the slot and press F9.

Gamepad
-------
Any gamepad that SDL3 knows (Xbox, PlayStation, Switch Pro, 8BitDo and others) works, also when you connect it
during the game. Button names of an Xbox pad (PlayStation: Cross = A, Square = X, Circle = B, Triangle = Y):
D-pad, left stick   move
A                   jump (Z)
X                   whip (X)
B + left/right      run (Up + Left/Right)
Y                   special weapon (Z + X at the same time; stand still)
Start               Return
Back / Select       pause (Esc)
RB / LB             game speed up / down (F1 / F10)

Saves
-----
The package is portable: because of the file portable.txt, the saves (F5, files state1.sav ... state9.sav) and the
game's own continue data go to the folder save/ja or save/en next to this program. The disk image is never changed.
If you delete portable.txt (or the folder cannot be written), the files go to the user data folder:
Linux ~/.local/share/rusty/ja or .../en ($XDG_DATA_HOME/rusty if it is set), Windows %APPDATA%\rusty\ja or ...\en.

Settings (environment variables)
--------------------------------
RUSTY_SCALE=<n>          window scale (default 2)
RUSTY_MUTE=1             no sound
RUSTY_RENDERER=gpu       GPU output with display filters (RUSTY_FILTER=sharp|nearest|scanlines,
                         RUSTY_ASPECT=16:10|4:3)
RUSTY_SAVE_DIR=<folder>  another save folder

Requirements
------------
Linux x86-64 (the SDL3 library is in lib/), or Windows 10 or newer, 64-bit (SDL3.dll is next to rusty.exe).

License
-------
The recompiler tools and the runtime of this project: MIT License (LICENSE). The game itself (C-Lab, 1993) and the
English translation (46 OkuMen) belong to their owners. This package contains no game data, but the program contains
code translated from the game; the MIT License does not cover that code.

Third-party software in this package:
- ymfm by Aaron Giles (YM2608 sound chip emulation, compiled into the program): BSD 3-Clause License,
  licenses/ymfm-LICENSE.txt.
- SDL3 by Sam Lantinga and the SDL contributors (window, input, sound; lib/libSDL3.so.0 or SDL3.dll, unchanged):
  zlib License, licenses/SDL3-LICENSE.txt.
- The GCC runtime libraries (libstdc++, libgcc) are linked into the program under the GCC Runtime Library Exception.
- Windows: the MinGW-w64 runtime (with winpthreads) is linked into the program: licenses/MinGW-w64-runtime-LICENSE.txt
  (only in the Windows package).
