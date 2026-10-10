/* Load and run a recompiled DOS program (COM or EXE) like DOS does: first the resident drivers of the link table
 * (RUSTY_DRIVERS="FILE.COM options;..." replaces the list, RUSTY_DRIVERS=none runs none), then the program named on
 * the command line. The first free segment is 0863, as in DOSBox-X. The exit code of the program is returned.
 * The command line names the game: a disk image (.hdi, .d88, .hdm), a folder with the game files or with disk images
 * (BOOT.COM starts), or a program file (with an optional command tail; the game folder is then RUSTY_GAME_DIR, else
 * the folder of the program). Without a game argument, the game in the folder of the program starts.
 * The code files of the game select the modules.
 * After the program ends, the resident drivers play for RUSTY_TAIL_SECONDS more (default 3, 0 = none). */
#include "rt.h"

#if defined(RUSTY_SDL) && defined(_WIN32)
#include <SDL3/SDL_main.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_FREE_SEGMENT 0x0863

static void run_driver(Cpu *c, const char *command)
{
    char name[64];
    const char *space = strchr(command, ' ');
    size_t length = space ? (size_t)(space - command) : strlen(command);
    if (length >= sizeof(name)) length = sizeof(name) - 1;
    memcpy(name, command, length);
    name[length] = 0;
    int code = dos_run(c, name, space ? space : "");
    if (code < 0) rt_fatal("driver %s: %s", name, code == -1 ? "file not found" : "no recompiled module");
    if (!(code & 0x300)) rt_log("driver %s did not stay resident (code %d)", name, code & 0xFF);
}

static void run_drivers(Cpu *c)
{
    const char *setting = getenv("RUSTY_DRIVERS");
    if (!setting) {
        for (const char *const *driver = rt_drivers; *driver; driver++) run_driver(c, *driver);
        return;
    }
    if (strcmp(setting, "none") == 0) return;
    char list[512];
    snprintf(list, sizeof(list), "%s", setting);
    for (char *item = strtok(list, ";"); item; item = strtok(0, ";")) run_driver(c, item);
}

/* The resident drivers keep playing after the program ends (in the game the next program loads meanwhile). */
static void play_tail(Cpu *c)
{
    const char *setting = getenv("RUSTY_TAIL_SECONDS");
    double seconds = setting ? atof(setting) : 3.0;
    if (seconds <= 0 || !rt_vector_is_recompiled(c, 0x60)) return;
    rt_log("program ended: the drivers play for %.1f s more", seconds);
    c->flags |= F_IF;
    for (long step = 0; step < (long)(seconds * 100000); step++) pc98_idle(c);
}

int main(int argc, char **argv)
{
    platform_setup_log();
    rt_log("rusty %s", RUSTY_VERSION);
    char found[1024];
    const char *game = argc > 1 ? argv[1] : 0;
    if (!game) {
        char folder[512];
        rt_program_folder(folder, sizeof(folder));
        if (!folder[0] || !disk_find_game(folder, found, sizeof(found))) {
            fprintf(stderr,
                    "usage: %s <disk image | game folder | program file [command tail]>\n"
                    "(the game files are loaded as data; code runs from the C translation)\n",
                    argv[0]);
            rt_fatal("No game found in %s.\n\nPut the disk image (.hdi) next to the program, or start the program "
                     "with the image as argument (drag the image onto the program).",
                     folder[0] ? folder : ".");
        }
        game = found;
        rt_log("no game argument: %s", game);
    }
    const char *slash = strrchr(game, '/');
    const char *backslash = strrchr(game, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    const char *name = slash ? slash + 1 : game;
    int whole_game = disk_open_game(game);
    if (!whole_game) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - game) : 1, slash ? game : ".");
        const char *setting = getenv("RUSTY_GAME_DIR");
        disk_use_folder(setting ? setting : dir);
    }
    rt_select_modules();
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem = calloc(RT_MEMORY_SIZE, 1);
    if (!cpu.mem) rt_fatal("out of memory");
    Cpu *c = &cpu;
    pc98_init(c);
    rt_install_bios_stubs(c);
    dos_init(c, FIRST_FREE_SEGMENT);
    run_drivers(c);
    bios_init(c);
    int code = whole_game ? dos_run(c, "BOOT.COM", "")
                          : dos_run_file(c, fopen(game, "rb"), name, argc > 2 ? argv[2] : "");
    if (code == -1) rt_fatal("cannot open %s", game);
    if (code == -2) rt_fatal("%s: no recompiled module matches this file", game);
    play_tail(c);
    return code & 0xFF;
}
