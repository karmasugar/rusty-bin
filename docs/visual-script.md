# VISUAL.COM cut scene script format

`VISUAL.COM` plays the cut scenes. Entries 0-5 play scenes 1-6 (scripts at `0632h`, `1462h`, `1800h`, `1A0Dh`,
`1E30h`, `2230h`), entry 6 plays the ending (script at `2940h`). The interpreter is `script_run` (`40A6h`).

A script is a byte stream:

- `00`: end of script.
- `20h` and more: text. Shift-JIS lead bytes (`81h`-`9Fh`, `E0h`-`EFh`) take 2 bytes. `draw_text_char` draws it.
- `01h`-`1Ah`: commands with operands. After every command the interpreter checks the skip key.

Operand types: `b` byte, `w` word (little endian), `S` file name = disk number byte + zero-terminated name.

| Code | Operands | Meaning |
|---|---|---|
| `01` | w w w w | copy screen rectangle (4 words; face animation frame) (guess) |
| `03` | b | set text attribute (b) |
| `04` | w | set text position (w) |
| `05` | b | wait b frames (skip key checked) |
| `06` | w w w b | draw window (w w w b) (guess) |
| `07` | S w w w | load image S at (w w w) |
| `08` | S w w w | load image S (w w w), variant |
| `09` | b w | graphics op (b w) (guess) |
| `0A` | b | graphics op (b) (guess) |
| `0B` | w | set palette/image param (w) (guess) |
| `0C` | w | set palette/image param (w) (guess) |
| `0D` | - | text line feed / scroll |
| `0E` | S | load animation script S (.ani) |
| `0F` | w b w | animation / face control (w b w) (guess) |
| `10` | S S | music: stop, load song S1/S2, start |
| `11` | S w w b | load image part S at (w w) slot b |
| `12` | w w | (w w) (guess) |
| `13` | w | call native code at w |
| `14` | b | play sound effect b |
| `15` | b b | text id and color (b b) |
| `16` | - | wait for a key |
| `17` | - | reset a buffer pointer (guess) |
| `18` | w | wait until timer reaches w (guess: music sync) |
| `19` | b w | (b w) (guess) |
| `1A` | - | page flip / redraw (guess) |

Checked: with these formats all 6 scene scripts and the ending script parse to their end byte.

Command `13h` calls native code at a script-given address. The scripts use it 3 times: `0140h` (scene 3),
`03CAh` and `0406h` (ending). The recompiler must treat these addresses as known call targets.
Command `01h` reads its 4 words itself (the interpreter steps back one byte before the call). (guess)
