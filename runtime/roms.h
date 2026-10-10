/* User-supplied ROM files (roms.c): the kanji font ROM and the YM2608 rhythm ROM are not part of the game. */
#ifndef RUSTY_ROMS_H
#define RUSTY_ROMS_H

#include <stddef.h>

#define ROM_FOUND 1
#define ROM_MISSING 0
#define ROM_DISABLED (-1)

/* setting: environment variable with an explicit file ("" = no ROM: ROM_DISABLED); name: the file name (any case)
 * in the ROM folders; extra_dir: one more folder searched last (0 = none). ROM_FOUND: the path is in out;
 * ROM_MISSING: out lists the searched folders, for the message. */
int rom_find(const char *setting, const char *name, const char *extra_dir, char *out, size_t size);

#endif
