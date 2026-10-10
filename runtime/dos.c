/* DOS emulation for recompiled programs: files, memory blocks with owners, vectors, program loading.
 * dos_run / EXEC (4Bh) load a COM or EXE file into the largest free block with a PSP and an environment, find the
 * recompiled module by the image signature hash and run it; exit (4Ch, INT 20h) and keep resident (31h, INT 27h) return
 * to the caller through longjmp. EXEC of a file without a recompiled module only logs it and returns a code from
 * RUSTY_EXEC_CODES. */
#include "rt.h"

#include <dirent.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MEMORY_TOP 0xA000
#define MAX_BLOCKS 64
#define MAX_HANDLES 20
#define MAX_DEPTH 8
#define ENV_PARAGRAPHS 0x10

typedef struct {
    uint16_t segment;
    uint16_t paragraphs;
    int used;
    uint16_t owner;
} Block;

/* An open DOS file: a host file (save folder, game folder), or an original file of a disk image in memory
 * (read only). */
typedef struct {
    FILE *file;
    const uint8_t *data;
    long size;
    long position;
    int used;
} Handle;

typedef struct {
    jmp_buf jump;
    uint16_t psp;
    int code;
} Run;

static Block blocks[MAX_BLOCKS];
static int block_count;
static uint16_t first_segment;
static Handle handles[MAX_HANDLES];
static uint8_t last_return_code;
static int exec_count;
static uint16_t reserved_top = MEMORY_TOP;
static uint16_t current_psp;
static Run runs[MAX_DEPTH];
static int run_depth;

uint16_t dos_reserve(uint16_t paragraphs)
{
    reserved_top = (uint16_t)(reserved_top - paragraphs);
    return reserved_top;
}

void dos_init(Cpu *c, uint16_t first_free)
{
    (void)c;
    block_count = 0;
    first_segment = first_free;
}

static int find_block(uint16_t segment)
{
    for (int i = 0; i < block_count; i++) {
        if (blocks[i].used && blocks[i].segment == segment) return i;
    }
    return -1;
}

static uint16_t block_end(int i)
{
    return (uint16_t)(blocks[i].segment + blocks[i].paragraphs);
}

static uint16_t next_used_after(uint16_t segment)
{
    uint16_t limit = reserved_top;
    for (int i = 0; i < block_count; i++) {
        if (blocks[i].used && blocks[i].segment > segment && blocks[i].segment < limit) limit = blocks[i].segment;
    }
    return limit;
}

static void dos_alloc(Cpu *c)
{
    uint16_t best = 0;
    uint16_t largest = 0;
    uint16_t candidate = first_segment;
    for (;;) {
        int inside = -1;
        for (int i = 0; i < block_count; i++) {
            if (blocks[i].used && candidate >= blocks[i].segment && candidate < block_end(i)) inside = i;
        }
        if (inside >= 0) {
            candidate = block_end(inside);
            continue;
        }
        if (candidate >= reserved_top) break;
        uint16_t gap = (uint16_t)(next_used_after(candidate) - candidate);
        if (gap > largest) largest = gap;
        if (gap >= c->bx.x && !best) best = candidate;
        candidate = (uint16_t)(candidate + gap);
    }
    if (!best || block_count >= MAX_BLOCKS) {
        rt_log("alloc %04x paragraphs: fail (largest %04x)", c->bx.x, largest);
        c->ax.x = 8;
        c->bx.x = largest;
        c->flags |= F_CF;
        return;
    }
    blocks[block_count++] = (Block){best, c->bx.x, 1, current_psp};
    rt_log("alloc %04x paragraphs -> %04x", c->bx.x, best);
    c->ax.x = best;
    c->flags &= ~F_CF;
}

static void dos_free(Cpu *c)
{
    int i = find_block(c->es);
    if (i < 0) {
        c->ax.x = 9;
        c->flags |= F_CF;
        return;
    }
    blocks[i] = blocks[--block_count];
    c->flags &= ~F_CF;
}

static void dos_resize(Cpu *c)
{
    int i = find_block(c->es);
    if (i < 0) {
        c->ax.x = 9;
        c->flags |= F_CF;
        return;
    }
    uint16_t available = (uint16_t)(next_used_after(blocks[i].segment) - blocks[i].segment);
    if (c->bx.x > available) {
        c->bx.x = available;
        c->ax.x = 8;
        c->flags |= F_CF;
    } else {
        blocks[i].paragraphs = c->bx.x;
        c->flags &= ~F_CF;
    }
    rt_log("resize block %04x -> CF=%d BX=%04x", c->es, FLAG_CF(c), c->bx.x);
}

static void read_string(Cpu *c, uint16_t seg, uint16_t off, char *out, size_t size, char terminator)
{
    size_t n = 0;
    while (n + 1 < size) {
        uint8_t ch = rd8(c, seg, (uint16_t)(off + n));
        if (ch == (uint8_t)terminator) break;
        out[n++] = (char)ch;
    }
    out[n] = 0;
}

static const char *base_name(const char *dos_name)
{
    const char *base = dos_name;
    for (const char *p = dos_name; *p; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    }
    return base;
}

/* Find base in dir (case-insensitive); out gets dir/<real name> or dir/<base> when it does not exist. */
static int find_in(const char *dir, const char *base, char *out, size_t size)
{
    DIR *listing = opendir(dir);
    int found = 0;
    if (listing) {
        struct dirent *item;
        while ((item = readdir(listing))) {
            if (strcasecmp(item->d_name, base) == 0) {
                snprintf(out, size, "%s/%s", dir, item->d_name);
                found = 1;
                break;
            }
        }
        closedir(listing);
    }
    if (!found) snprintf(out, size, "%s/%s", dir, base);
    return found;
}

static void make_dirs(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        platform_make_folder(path);
        *p = '/';
    }
    platform_make_folder(path);
}

#ifdef RUSTY_RELEASE
/* Portable mode: a file portable.txt next to the program puts the files into <program folder>/save/<ja|en> (as in
 * Zelda 64: Recompiled and Unleashed Recompiled); 0 when there is no marker or the folder cannot be written. */
static int portable_dir(char *dir, size_t size)
{
    char program[512];
    char marker[600];
    rt_program_folder(program, sizeof(program));
    snprintf(marker, sizeof(marker), "%s/portable.txt", program);
    if (!program[0] || !platform_exists(marker)) return 0;
    snprintf(dir, size, "%.500s/save/%s", program, rt_game_version_tag());
    make_dirs(dir);
    if (platform_writable(dir)) return 1;
    rt_log("portable.txt found, but %s cannot be written: the files go to the user data folder", dir);
    return 0;
}
#endif

/* The folder for the files the runtime writes: build/recomp (dev builds, run from the project root). A release
 * build uses <program folder>/save/<ja|en> in portable mode, else the user data folder ($XDG_DATA_HOME or
 * ~/.local/share)/rusty/<ja|en>; one folder per game version. */
const char *dos_user_dir(void)
{
#ifdef RUSTY_RELEASE
    static char dir[512];
    if (!dir[0] && !portable_dir(dir, sizeof(dir))) {
        char base[400];
        platform_user_data_folder(base, sizeof(base));
        snprintf(dir, sizeof(dir), "%s/rusty/%s", base, rt_game_version_tag());
        make_dirs(dir);
    }
    return dir;
#else
    static int made;
    if (!made) {
        make_dirs("build/recomp");
        made = 1;
    }
    return "build/recomp";
#endif
}

const char *dos_save_dir(void)
{
    static char dir[512];
    const char *setting = getenv("RUSTY_SAVE_DIR");
    if (setting) return setting;
    if (!dir[0] && snprintf(dir, sizeof(dir), "%s/savedata", dos_user_dir()) >= (int)sizeof(dir))
        rt_fatal("the save folder path is too long: %s/savedata", dos_user_dir());
    return dir;
}

static void make_save_dir(void)
{
    make_dirs(dos_save_dir());
}

/* Files the game writes go to the save folder (RUSTY_SAVE_DIR, default build/recomp/savedata); reads take a file
 * from there first, then the original game files (disk.c: a folder or disk images), which stay unchanged. */
static int open_original(const char *base, Handle *handle)
{
    DiskFile file;
    if (!disk_open_file(base, &file)) return 0;
    *handle = (Handle){file.file, file.data, file.size, 0, 1};
    return 1;
}

static int open_host(const char *path, const char *mode, Handle *handle)
{
    FILE *file = fopen(path, mode);
    *handle = (Handle){file, 0, 0, 0, file != 0};
    return file != 0;
}

static int open_for_read(const char *dos_name, Handle *handle)
{
    const char *base = base_name(dos_name);
    char path[512];
    if (find_in(dos_save_dir(), base, path, sizeof(path))) return open_host(path, "rb", handle);
    return open_original(base, handle);
}

static long handle_read(Handle *handle, uint8_t *buffer, long count)
{
    if (handle->file) return (long)fread(buffer, 1, (size_t)count, handle->file);
    long left = handle->size - handle->position;
    long done = count < left ? count : left > 0 ? left : 0;
    memcpy(buffer, handle->data + handle->position, (size_t)done);
    handle->position += done;
    return done;
}

static long handle_seek(Handle *handle, long offset, int origin)
{
    if (handle->file) {
        fseek(handle->file, offset, origin);
        return ftell(handle->file);
    }
    long base = origin == SEEK_CUR ? handle->position : origin == SEEK_END ? handle->size : 0;
    handle->position = base + offset < 0 ? 0 : base + offset;
    return handle->position;
}

static void handle_close(Handle *handle)
{
    if (handle->file) fclose(handle->file);
    *handle = (Handle){0, 0, 0, 0, 0};
}

static void copy_file(Handle *in, const char *to)
{
    FILE *out = fopen(to, "wb");
    uint8_t chunk[4096];
    long count;
    while (out && (count = handle_read(in, chunk, sizeof(chunk))) > 0) fwrite(chunk, 1, (size_t)count, out);
    handle_close(in);
    if (out) fclose(out);
}

/* The content the game would read now (save folder first, then the original); -1 when the file does not exist. */
long dos_read_game_file(const char *name, uint8_t *buffer, size_t capacity)
{
    Handle handle;
    if (!open_for_read(name, &handle)) return -1;
    long size = handle_read(&handle, buffer, (long)capacity);
    handle_close(&handle);
    return size;
}

/* The original game file (not the save folder); -1 when it does not exist. */
long dos_read_original_file(const char *name, uint8_t *buffer, size_t capacity)
{
    Handle handle;
    if (!open_original(base_name(name), &handle)) return -1;
    long size = handle_read(&handle, buffer, (long)capacity);
    handle_close(&handle);
    return size;
}

/* Put a file into the save folder (size < 0: remove it there). */
void dos_write_save_file(const char *name, const uint8_t *data, long size)
{
    char path[512];
    make_save_dir();
    find_in(dos_save_dir(), base_name(name), path, sizeof(path));
    if (size < 0) {
        remove(path);
        return;
    }
    FILE *file = fopen(path, "wb");
    if (!file) rt_fatal("cannot write %s", path);
    fwrite(data, 1, (size_t)size, file);
    fclose(file);
}

static int writable_path(const char *dos_name, char *out, size_t size, int keep_content)
{
    const char *base = base_name(dos_name);
    make_save_dir();
    if (find_in(dos_save_dir(), base, out, size)) return 1;
    Handle original;
    if (keep_content && open_original(base, &original)) {
        copy_file(&original, out);
        return 1;
    }
    return 0;
}

static int new_handle(Handle *handle)
{
    for (int h = 5; h < MAX_HANDLES; h++) {
        if (!handles[h].used) {
            handles[h] = *handle;
            return h;
        }
    }
    handle_close(handle);
    return -1;
}

static void dos_open(Cpu *c, int create)
{
    char name[128];
    char path[512];
    read_string(c, c->ds, c->dx.x, name, sizeof(name), 0);
    Handle file = {0, 0, 0, 0, 0};
    if (create) {
        writable_path(name, path, sizeof(path), 0);
        open_host(path, "w+b", &file);
    } else if (c->ax.b.l & 3) {
        if (writable_path(name, path, sizeof(path), 1)) open_host(path, "r+b", &file);
    } else {
        open_for_read(name, &file);
    }
    if (file.used) rt_note_open(base_name(name));
    int handle = file.used ? new_handle(&file) : -1;
    if (handle < 0) {
        rt_log("%s \"%s\": not found", create ? "create" : "open", name);
        c->ax.x = 2;
        c->flags |= F_CF;
        return;
    }
    rt_log("%s \"%s\" -> handle %d (frame %ld)", create ? "create" : "open", name, handle, pc98_frame());
    c->ax.x = (uint16_t)handle;
    c->flags &= ~F_CF;
}

static Handle *handle_file(Cpu *c)
{
    uint16_t handle = c->bx.x;
    if (handle >= MAX_HANDLES || !handles[handle].used) {
        c->ax.x = 6;
        c->flags |= F_CF;
        return 0;
    }
    return &handles[handle];
}

static void dos_read(Cpu *c)
{
    Handle *file = handle_file(c);
    if (!file) return;
    static uint8_t buffer[0x10000];
    long done = handle_read(file, buffer, c->cx.x);
    for (long i = 0; i < done; i++) wr8(c, c->ds, (uint16_t)(c->dx.x + i), buffer[i]);
    c->ax.x = (uint16_t)done;
    c->flags &= ~F_CF;
}

static void dos_write(Cpu *c)
{
    if (c->bx.x == 1 || c->bx.x == 2) {
        for (uint16_t i = 0; i < c->cx.x; i++) console_write(c, rd8(c, c->ds, (uint16_t)(c->dx.x + i)));
        c->ax.x = c->cx.x;
        c->flags &= ~F_CF;
        return;
    }
    Handle *file = handle_file(c);
    if (!file) return;
    if (!file->file) {
        c->ax.x = 5;
        c->flags |= F_CF;
        return;
    }
    for (uint16_t i = 0; i < c->cx.x; i++) fputc(rd8(c, c->ds, (uint16_t)(c->dx.x + i)), file->file);
    c->ax.x = c->cx.x;
    c->flags &= ~F_CF;
}

static void dos_seek(Cpu *c)
{
    Handle *file = handle_file(c);
    if (!file) return;
    int32_t offset = (int32_t)(((uint32_t)c->cx.x << 16) | c->dx.x);
    int origin = c->ax.b.l == 1 ? SEEK_CUR : c->ax.b.l == 2 ? SEEK_END : SEEK_SET;
    long position = handle_seek(file, origin == SEEK_SET ? (long)(uint32_t)offset : (long)offset, origin);
    c->ax.x = (uint16_t)position;
    c->dx.x = (uint16_t)(position >> 16);
    c->flags &= ~F_CF;
}

static void dos_close(Cpu *c)
{
    Handle *file = handle_file(c);
    if (!file) return;
    handle_close(file);
    c->flags &= ~F_CF;
}

static int exec_return_code(const char *program)
{
    const char *table = getenv("RUSTY_EXEC_CODES");
    if (!table) table = "ONGCHK.COM=3";
    const char *base = strrchr(program, '\\');
    base = base ? base + 1 : program;
    size_t length = strlen(base);
    for (const char *p = table; *p;) {
        const char *end = strchr(p, ',');
        size_t item = end ? (size_t)(end - p) : strlen(p);
        if (item > length && strncasecmp(p, base, length) == 0 && p[length] == '=') return atoi(p + length + 1);
        p += item + (end ? 1 : 0);
    }
    return 0;
}

static uint16_t header_word(const uint8_t *data, size_t offset)
{
    return (uint16_t)(data[offset] | (data[offset + 1] << 8));
}

static uint16_t largest_free(uint16_t *size)
{
    uint16_t best = 0;
    *size = 0;
    uint16_t candidate = first_segment;
    while (candidate < reserved_top) {
        int inside = -1;
        for (int i = 0; i < block_count; i++) {
            if (blocks[i].used && candidate >= blocks[i].segment && candidate < block_end(i)) inside = i;
        }
        if (inside >= 0) {
            candidate = block_end(inside);
            continue;
        }
        uint16_t gap = (uint16_t)(next_used_after(candidate) - candidate);
        if (gap > *size) {
            *size = gap;
            best = candidate;
        }
        candidate = (uint16_t)(candidate + gap);
    }
    return best;
}

static uint16_t new_block(uint16_t paragraphs, uint16_t owner)
{
    uint16_t size;
    uint16_t segment = largest_free(&size);
    if (!segment || size < paragraphs || block_count >= MAX_BLOCKS) rt_fatal("no memory for %04x paragraphs", paragraphs);
    blocks[block_count++] = (Block){segment, paragraphs, 1, owner};
    return segment;
}

static void free_owned(uint16_t psp)
{
    for (int i = 0; i < block_count;) {
        if (blocks[i].owner == psp) {
            blocks[i] = blocks[--block_count];
        } else {
            i++;
        }
    }
}

static void end_program(Cpu *c, int code, int keep_paragraphs)
{
    if (!run_depth) rt_fatal("program end outside of a program run");
    Run *run = &runs[run_depth - 1];
    if (keep_paragraphs >= 0) {
        int i = find_block(run->psp);
        if (i >= 0) blocks[i].paragraphs = (uint16_t)(keep_paragraphs < 6 ? 6 : keep_paragraphs);
        rt_log("keep resident: PSP %04x, %04x paragraphs, code %d", run->psp, blocks[i >= 0 ? i : 0].paragraphs, code);
    } else {
        rt_log("exit with code %d", code);
    }
    (void)c;
    run->code = code | (keep_paragraphs >= 0 ? 0x300 : 0);
    longjmp(run->jump, 1);
}

static uint8_t program_buffer[0x80000];

static int run_loaded(Cpu *c, size_t size, const char *name, const char *tail);

int dos_run(Cpu *c, const char *name, const char *tail)
{
    Handle handle;
    if (!open_for_read(name, &handle)) return -1;
    long size = handle_read(&handle, program_buffer, sizeof(program_buffer));
    handle_close(&handle);
    return run_loaded(c, (size_t)size, base_name(name), tail);
}

/* Run the program in file (closed here; 0 = not found): -1 not found, -2 no recompiled module, else the exit code
 * (0x300 set when it stays resident). */
int dos_run_file(Cpu *c, FILE *file, const char *name, const char *tail)
{
    if (!file) return -1;
    size_t size = fread(program_buffer, 1, sizeof(program_buffer), file);
    fclose(file);
    return run_loaded(c, size, name, tail);
}

static int run_loaded(Cpu *c, size_t size, const char *name, const char *tail)
{
    static uint8_t unpacked[0x80000];
    uint8_t *buffer = program_buffer;
    const uint8_t *data = buffer;
    if (exepack_detect(buffer, size)) {
        size_t unpacked_size = exepack_unpack(buffer, size, unpacked, sizeof(unpacked));
        if (!unpacked_size) rt_fatal("%s: EXEPACK data is corrupt", name);
        rt_log("%s: EXEPACK unpacked (%zu -> %zu bytes)", name, size, unpacked_size);
        data = unpacked;
        size = unpacked_size;
    }
    int exe = size >= 0x1C && data[0] == 'M' && data[1] == 'Z';
    uint16_t parent = current_psp;

    uint16_t env = new_block(ENV_PARAGRAPHS, 0xFFFF);
    char environment[160];
    int length = snprintf(environment, sizeof(environment), "PATH=A:\\%cCOMSPEC=A:\\COMMAND.COM%c%c%c%cA:\\RUSTY\\%s", 0,
                          0, 0, 1, 0, name);
    for (int i = 0; i <= length; i++) wr8(c, env, (uint16_t)i, (uint8_t)environment[i]);

    uint16_t free_size;
    largest_free(&free_size);
    uint16_t paragraphs = free_size;
    size_t image_size = size;
    uint16_t header_paragraphs = 0;
    if (exe) {
        uint16_t last_page = header_word(data, 2), pages = header_word(data, 4);
        header_paragraphs = header_word(data, 8);
        size_t file_size = (size_t)pages * 512 - (last_page ? 512 - last_page : 0);
        image_size = file_size - (size_t)header_paragraphs * 16;
        uint32_t wanted = 0x10 + (uint32_t)((image_size + 15) / 16) + header_word(data, 0x0C);
        if (wanted < paragraphs) paragraphs = (uint16_t)wanted;
    }
    uint16_t psp = new_block(paragraphs, 0);
    blocks[block_count - 1].owner = psp;
    for (int i = 0; i < block_count; i++) {
        if (blocks[i].segment == env) blocks[i].owner = psp;
    }
    for (int i = 0; i < 0x100; i++) wr8(c, psp, (uint16_t)i, 0);
    wr8(c, psp, 0x00, 0xCD);
    wr8(c, psp, 0x01, 0x20);
    wr16(c, psp, 0x02, (uint16_t)(psp + paragraphs));
    wr16(c, psp, 0x16, parent ? parent : psp);
    wr16(c, psp, 0x2C, env);
    size_t tail_length = strlen(tail);
    if (tail_length > 126) tail_length = 126;
    wr8(c, psp, 0x80, (uint8_t)tail_length);
    for (size_t i = 0; i < tail_length; i++) wr8(c, psp, (uint16_t)(0x81 + i), (uint8_t)tail[i]);
    wr8(c, psp, (uint16_t)(0x81 + tail_length), 0x0D);

    uint16_t load_seg, cs, ip, ss, sp;
    const RtModule *module;
    if (exe) {
        load_seg = (uint16_t)(psp + 0x10);
        memcpy(c->mem + rt_linear(load_seg, 0), data + (size_t)header_paragraphs * 16, image_size);
        module = rt_find_module(c, load_seg);
        uint16_t relocations = header_word(data, 6), relocation_offset = header_word(data, 0x18);
        for (uint16_t i = 0; i < relocations; i++) {
            uint16_t offset = header_word(data, (size_t)relocation_offset + i * 4u);
            uint16_t target = (uint16_t)(load_seg + header_word(data, (size_t)relocation_offset + i * 4u + 2));
            wr16(c, target, offset, (uint16_t)(rd16(c, target, offset) + load_seg));
        }
        ss = (uint16_t)(load_seg + header_word(data, 0x0E));
        sp = header_word(data, 0x10);
        ip = header_word(data, 0x14);
        cs = (uint16_t)(load_seg + header_word(data, 0x16));
    } else {
        load_seg = psp;
        memcpy(c->mem + rt_linear(psp, 0x100), data, size < 0xFF00 ? size : 0xFF00);
        module = rt_find_module(c, load_seg);
        cs = ss = psp;
        ip = 0x100;
        sp = 0xFFFE;
    }
    if (!module) {
        free_owned(psp);
        return -2;
    }
    uint16_t module_paragraphs = exe ? (uint16_t)((image_size + 15) / 16) : (paragraphs < 0x1000 ? paragraphs : 0x1000);
    rt_register_instance(module, load_seg, module_paragraphs);
    rt_log("run %s \"%s\": %s at %04x (PSP %04x, %04x paragraphs)", name, tail, module->name, load_seg, psp, paragraphs);

    if (run_depth >= MAX_DEPTH) rt_fatal("programs nested too deep");
    Cpu saved = *c;
    Run *run = &runs[run_depth++];
    run->psp = psp;
    run->code = 0;
    current_psp = psp;
    if (!setjmp(run->jump)) {
        c->ax.x = c->bx.x = c->cx.x = c->dx.x = 0;
        c->si = c->di = c->bp = 0;
        c->ds = c->es = psp;
        c->ss = ss;
        c->sp = sp;
        c->flags = 0x0202;
        if (exe) {
            push16(c, psp);
            push16(c, 0x0000);
        } else {
            wr16(c, ss, sp, 0);
            push16(c, 0x0000);
        }
        rt_start(c, cs, ip);
        rt_log("%s returned: program end", module->name);
    }
    run_depth--;
    int code = run->code;
    current_psp = parent;
    if (code & 0x300) {
        int kept = find_block(psp);
        uint16_t size = kept >= 0 ? blocks[kept].paragraphs : 0;
        uint16_t module_size = (uint16_t)(size - (load_seg - psp));
        if (module_size < module_paragraphs) rt_resize_instance(load_seg, module_size);
    } else {
        rt_unregister_instance(load_seg);
        free_owned(psp);
    }
    uint8_t *memory = c->mem;
    uint64_t icount = c->icount;
    *c = saved;
    c->mem = memory;
    c->icount = icount;
    last_return_code = (uint8_t)code;
    return code & 0x3FF;
}

static void dos_exec(Cpu *c)
{
    char program[128];
    char tail[130];
    read_string(c, c->ds, c->dx.x, program, sizeof(program), 0);
    uint16_t tail_off = rd16(c, c->es, (uint16_t)(c->bx.x + 2));
    uint16_t tail_seg = rd16(c, c->es, (uint16_t)(c->bx.x + 4));
    uint8_t length = rd8(c, tail_seg, tail_off);
    for (int i = 0; i < length && i < 127; i++) tail[i] = (char)rd8(c, tail_seg, (uint16_t)(tail_off + 1 + i));
    tail[length < 127 ? length : 127] = 0;
    const char *base = base_name(program);
    int result = dos_run(c, base, tail);
    if (result == -1) {
        rt_log("EXEC %s: file not found", program);
        c->ax.x = 2;
        c->flags |= F_CF;
        return;
    }
    if (result == -2) {
        const char *limit = getenv("RUSTY_EXEC_LIMIT");
        if (++exec_count > (limit ? atoi(limit) : 20)) {
            rt_log("EXEC limit reached: stop");
            exit(0);
        }
        last_return_code = (uint8_t)exec_return_code(program);
        printf("EXEC %-14s tail=\"%s\" -> return code %u\n", program, tail, last_return_code);
    }
    c->flags &= ~F_CF;
}

/* Emulated time of the real MS-DOS code for a call, measured once in DOSBox-X (median per function from the logo
 * and the opening): the open searches the directory on the disk. */
static double dos_call_cost_us(uint8_t function)
{
    switch (function) {
    case 0x3D: return 4140;
    case 0x3F: return 290;
    case 0x3B: return 700;
    case 0x09: return 850;
    case 0x48: return 42;
    case 0x3E: return 26;
    case 0x40: return 23;
    case 0x4A: return 19;
    case 0x42: return 17;
    case 0x44: return 17;
    case 0x49: return 15;
    case 0x35: return 13;
    case 0x25: return 12;
    default: return 15;
    }
}

static void dos_int21(Cpu *c)
{
    uint8_t function = c->ax.b.h;
    switch (function) {
    case 0x02:
        console_write(c, c->dx.b.l);
        break;
    case 0x06:
        if (c->dx.b.l != 0xFF) console_write(c, c->dx.b.l);
        break;
    case 0x09:
        for (uint16_t off = c->dx.x; rd8(c, c->ds, off) != '$'; off++) console_write(c, rd8(c, c->ds, off));
        break;
    case 0x0E:
        c->ax.b.l = 4;
        break;
    case 0x19:
        c->ax.b.l = 0;
        break;
    case 0x25:
        wr16(c, 0, (uint16_t)(c->ax.b.l * 4), c->dx.x);
        wr16(c, 0, (uint16_t)(c->ax.b.l * 4 + 2), c->ds);
        rt_log("set vector %02xh = %04x:%04x", c->ax.b.l, c->ds, c->dx.x);
        break;
    case 0x2A: {
        time_t now = (time_t)pc98_clock_seconds();
        struct tm *t = localtime(&now);
        c->cx.x = (uint16_t)(t->tm_year + 1900);
        c->dx.b.h = (uint8_t)(t->tm_mon + 1);
        c->dx.b.l = (uint8_t)t->tm_mday;
        c->ax.b.l = (uint8_t)t->tm_wday;
        break;
    }
    case 0x2C: {
        time_t now = (time_t)pc98_clock_seconds();
        struct tm *t = localtime(&now);
        c->cx.b.h = (uint8_t)t->tm_hour;
        c->cx.b.l = (uint8_t)t->tm_min;
        c->dx.b.h = (uint8_t)t->tm_sec;
        c->dx.b.l = 0;
        break;
    }
    case 0x30:
        c->ax.x = 0x1406;
        c->bx.x = 0;
        break;
    case 0x33:
        if (c->ax.b.l == 0) c->dx.b.l = 0;
        break;
    case 0x35:
        c->bx.x = rd16(c, 0, (uint16_t)(c->ax.b.l * 4));
        c->es = rd16(c, 0, (uint16_t)(c->ax.b.l * 4 + 2));
        break;
    case 0x3B:
        c->flags &= ~F_CF;
        break;
    case 0x3C:
        dos_open(c, 1);
        break;
    case 0x3D:
        dos_open(c, 0);
        break;
    case 0x3E:
        dos_close(c);
        break;
    case 0x3F:
        dos_read(c);
        break;
    case 0x40:
        dos_write(c);
        break;
    case 0x42:
        dos_seek(c);
        break;
    case 0x44:
        if (c->ax.b.l == 0x00) c->dx.x = c->bx.x < 5 ? 0x80D3 : 0x0002;
        c->flags &= ~F_CF;
        break;
    case 0x48:
        dos_alloc(c);
        break;
    case 0x49:
        dos_free(c);
        break;
    case 0x4A:
        dos_resize(c);
        break;
    case 0x4B:
        dos_exec(c);
        break;
    case 0x4C:
        end_program(c, c->ax.b.l, -1);
        break;
    case 0x31:
        end_program(c, c->ax.b.l, c->dx.x);
        break;
    case 0x4D:
        c->ax.x = last_return_code;
        break;
    case 0x50:
        current_psp = c->bx.x;
        break;
    case 0x51:
    case 0x62:
        c->bx.x = current_psp;
        break;
    default:
        rt_fatal("unsupported INT 21h AH=%02xh", function);
    }
}

void rt_int(Cpu *c, uint8_t number)
{
    rt_changes++;
    if (rt_vector_is_recompiled(c, number)) {
        rt_call_interrupt(c, number);
        return;
    }
    if (number == 0x21) {
        uint8_t function = c->ax.b.h;
        dos_int21(c);
        pc98_charge_us(c, dos_call_cost_us(function));
        return;
    }
    if (number == 0x20) end_program(c, 0, -1);
    if (number == 0x27) end_program(c, 0, (c->dx.x + 15) / 16);
    if (rt_bios_int(c, number)) return;
    rt_fatal("unsupported INT %02xh (AX=%04x)", number, c->ax.x);
}

/* Snapshot: the memory blocks and the EXEC bookkeeping. The run stack itself (setjmp) is the same at every safe
 * point; dos_context lets a load check that. */
void dos_snapshot(StateStream *s)
{
    STATE(s, blocks);
    STATE(s, block_count);
    STATE(s, first_segment);
    STATE(s, last_return_code);
    STATE(s, exec_count);
    STATE(s, reserved_top);
}

int dos_open_handles(void)
{
    int count = 0;
    for (int h = 5; h < MAX_HANDLES; h++) count += handles[h].used;
    return count;
}

uint32_t dos_context(void)
{
    return (uint32_t)run_depth << 16 | current_psp;
}
