/* The few host system calls that differ between Linux and Windows: the folder of the program, folders and their
 * access, and the user data folder. All paths are UTF-8; on Windows the program manifest selects the UTF-8 code page,
 * so the C library file functions take UTF-8 paths too. */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

void rt_program_folder(char *out, size_t size)
{
    char path[1024];
#ifdef _WIN32
    wchar_t wide[1024];
    DWORD length = GetModuleFileNameW(0, wide, 1024);
    int converted = length > 0 && length < 1024
                        ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, path, (int)sizeof(path) - 1, 0, 0)
                        : 0;
    if (converted <= 0) {
        out[0] = 0;
        return;
    }
    path[converted] = 0;
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
#else
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) {
        out[0] = 0;
        return;
    }
    path[length] = 0;
#endif
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    snprintf(out, size, "%s", path);
}

void platform_make_folder(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

int platform_exists(const char *path)
{
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

int platform_writable(const char *path)
{
#ifdef _WIN32
    return _access(path, 2) == 0;
#else
    return access(path, W_OK) == 0;
#endif
}

/* rename() that also replaces an existing file (the C library rename of Windows fails then); 0 on success. */
int platform_replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* The base folder for user data: %APPDATA% on Windows, $XDG_DATA_HOME or ~/.local/share elsewhere. */
void platform_user_data_folder(char *out, size_t size)
{
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    snprintf(out, size, "%s", appdata && *appdata ? appdata : ".");
    for (char *p = out; *p; p++) {
        if (*p == '\\') *p = '/';
    }
#else
    const char *data = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (data && *data) {
        snprintf(out, size, "%s", data);
    } else {
        snprintf(out, size, "%s/.local/share", home ? home : ".");
    }
#endif
}

/* Windows GUI program started without a console or pipe: the log goes to the console it was started from, else to
 * rusty.log next to the program (or in %APPDATA%\rusty when the program folder cannot be written). A valid stderr
 * (console, pipe, file) and other systems stay as they are. */
void platform_setup_log(void)
{
#ifdef _WIN32
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error && error != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
        return;
    }
    char folder[600], path[700];
    rt_program_folder(folder, sizeof(folder));
    if (!folder[0] || !platform_writable(folder)) {
        char base[400];
        platform_user_data_folder(base, sizeof(base));
        snprintf(folder, sizeof(folder), "%s/rusty", base);
        platform_make_folder(folder);
    }
    snprintf(path, sizeof(path), "%s/rusty.log", folder);
    if (freopen(path, "w", stderr)) setvbuf(stderr, 0, _IONBF, 0);
#endif
}
