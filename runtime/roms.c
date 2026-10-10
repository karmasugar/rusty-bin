/* User-supplied ROM files, searched in this order: the file in the setting variable, then the folders
 * RUSTY_ROM_DIR, the folder of the game (or of its disk image), roms/ next to the program (a dev build links
 * build/linux-dev/roms to the roms/ folder of the project), the folder of the program, and the extra folder of the
 * caller. */
#include "roms.h"
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROM_DIRS 6

int rom_find(const char *setting, const char *name, const char *extra_dir, char *out, size_t size)
{
    const char *file = getenv(setting);
    if (file) {
        if (!*file) return ROM_DISABLED;
        snprintf(out, size, "%s", file);
        return ROM_FOUND;
    }
    char dirs[MAX_ROM_DIRS][512];
    int count = 0;
    const char *rom_dir = getenv("RUSTY_ROM_DIR");
    if (rom_dir) snprintf(dirs[count++], sizeof(dirs[0]), "%s", rom_dir);
    disk_host_folder(dirs[count++], sizeof(dirs[0]));
    char program[512];
    rt_program_folder(program, sizeof(program));
    if (*program) {
        snprintf(dirs[count++], sizeof(dirs[0]), "%.500s/roms", program);
        snprintf(dirs[count++], sizeof(dirs[0]), "%s", program);
    }
    if (extra_dir) snprintf(dirs[count++], sizeof(dirs[0]), "%s", extra_dir);
    for (int i = 0; i < count; i++) {
        if (dirs[i][0] && disk_host_file(dirs[i], name, out, size)) return ROM_FOUND;
    }
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < count && used < size; i++) {
        used += (size_t)snprintf(out + used, size - used, "%s%s", i ? ", " : "", dirs[i][0] ? dirs[i] : ".");
    }
    return ROM_MISSING;
}
