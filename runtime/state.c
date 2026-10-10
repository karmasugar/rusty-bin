/* Snapshot save states in slots 1-9 (state<n>.sav in RUSTY_STATE_DIR, default the user folder of dos_user_dir).
 * Save and load run only at a safe point: rt_safe_point, which the generated code calls at the top of JO.EXE's main
 * loop. There the C call chain is always BOOT.COM -> EXEC -> JO.EXE's main loop, and the recompiled functions keep
 * no state in C variables, so a load only overwrites the emulated machine (CPU, memory, devices) and continues; no C
 * frame has to be rebuilt. F5/F9 (state_request) wait for the next safe point; with no safe point for 60 frames
 * (title screen, menus) the request is dropped with a message. The file has a header (format, game version, stage,
 * time, a hash of the runtime context and of the payload); a load checks all of it before it changes anything.
 * Tests: RUSTY_STATE_SAVE_AT=<frame>:<slot> saves at the first safe point at or after that frame,
 * RUSTY_STATE_LOAD=<slot> loads at the first safe point. */
#include "rt.h"
#include "sound.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STATE_MAGIC "RUSTYSAV"
#define STATE_FORMAT 1
#define SLOT_COUNT 9
#define REQUEST_TIMEOUT_FRAMES 60
#define REQUEST_SAVE 1
#define REQUEST_LOAD 2

struct StateStream {
    int loading;
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t offset;
    int error;
};

typedef struct {
    char magic[8];
    uint32_t format;
    char version[4];
    int32_t stage;
    int64_t saved_at;
    uint64_t context;
    uint64_t payload_size;
    uint64_t payload_hash;
} StateHeader;

static int slot = 1;
static int request;
static long request_frame;
static long last_safe_frame = -1000;
static int settings_read;
static long save_at_frame = -1;
static int save_at_slot;
static int load_slot_at_start;

void state_io(StateStream *s, void *data, size_t size)
{
    if (s->error) return;
    if (s->loading) {
        if (s->offset + size > s->size) {
            s->error = 1;
            return;
        }
        memcpy(data, s->data + s->offset, size);
        s->offset += size;
        return;
    }
    if (s->size + size > s->capacity) {
        size_t capacity = s->capacity ? s->capacity * 2 : 0x200000;
        while (capacity < s->size + size) capacity *= 2;
        uint8_t *bigger = realloc(s->data, capacity);
        if (!bigger) {
            s->error = 1;
            return;
        }
        s->data = bigger;
        s->capacity = capacity;
    }
    memcpy(s->data + s->size, data, size);
    s->size += size;
}

int state_loading(const StateStream *s)
{
    return s->loading;
}

static uint64_t payload_hash(const uint8_t *data, size_t size)
{
    uint64_t value = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) value = (value ^ data[i]) * 0x100000001B3ull;
    return value;
}

static void cpu_state(Cpu *c, StateStream *s)
{
    state_io(s, &c->ax, sizeof(c->ax));
    state_io(s, &c->bx, sizeof(c->bx));
    state_io(s, &c->cx, sizeof(c->cx));
    state_io(s, &c->dx, sizeof(c->dx));
    state_io(s, &c->si, sizeof(c->si));
    state_io(s, &c->di, sizeof(c->di));
    state_io(s, &c->bp, sizeof(c->bp));
    state_io(s, &c->sp, sizeof(c->sp));
    state_io(s, &c->cs, sizeof(c->cs));
    state_io(s, &c->ds, sizeof(c->ds));
    state_io(s, &c->es, sizeof(c->es));
    state_io(s, &c->ss, sizeof(c->ss));
    state_io(s, &c->flags, sizeof(c->flags));
    state_io(s, &c->ret_ip, sizeof(c->ret_ip));
    state_io(s, &c->ret_cs, sizeof(c->ret_cs));
    state_io(s, &c->icount, sizeof(c->icount));
}

/* The whole snapshot, in one order for save and load. */
static void machine_state(Cpu *c, StateStream *s)
{
    cpu_state(c, s);
    state_io(s, c->mem, RT_MEMORY_SIZE);
    pc98_snapshot(s);
    video_snapshot(s);
    sound_snapshot(s);
    dos_snapshot(s);
    console_snapshot(s);
    display_snapshot(s);
    input_snapshot(s);
    cheats_snapshot(s);
    rt_snapshot(s);
}

static uint64_t context_hash(const Cpu *c)
{
    uint64_t value = rt_context_hash(c);
    uint32_t dos = dos_context();
    return (value ^ dos) * 0x100000001B3ull;
}

static void slot_path(int number, char *out, size_t size)
{
    const char *dir = getenv("RUSTY_STATE_DIR");
    snprintf(out, size, "%s/state%d.sav", dir && *dir ? dir : dos_user_dir(), number);
}

static void message(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void message(const char *format, ...)
{
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    rt_log("state: %s", text);
    display_message(text, 3);
}

static void save_slot(Cpu *c, int number)
{
    StateStream s = {0};
    machine_state(c, &s);
    if (s.error) {
        free(s.data);
        message("Slot %d: not saved (out of memory)", number);
        return;
    }
    StateHeader header = {0};
    memcpy(header.magic, STATE_MAGIC, sizeof(header.magic));
    header.format = STATE_FORMAT;
    snprintf(header.version, sizeof(header.version), "%s", rt_game_version_tag());
    header.stage = rt_current_story();
    header.saved_at = (int64_t)time(0);
    header.context = context_hash(c);
    header.payload_size = s.size;
    header.payload_hash = payload_hash(s.data, s.size);
    char path[600], temporary[620];
    slot_path(number, path, sizeof(path));
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    FILE *file = fopen(temporary, "wb");
    int written = file && fwrite(&header, sizeof(header), 1, file) == 1 && fwrite(s.data, 1, s.size, file) == s.size;
    if (file && fclose(file) != 0) written = 0;
    free(s.data);
    if (!written || platform_replace_file(temporary, path) != 0) {
        remove(temporary);
        message("Slot %d: cannot write %s", number, path);
        return;
    }
    message("Saved to slot %d (stage %d)", number, header.stage);
}

static uint8_t *read_slot(int number, StateHeader *header, size_t *size)
{
    char path[600];
    slot_path(number, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    uint8_t *data = 0;
    if (fread(header, sizeof(*header), 1, file) == 1 && memcmp(header->magic, STATE_MAGIC, 8) == 0 &&
        header->payload_size < 0x10000000) {
        data = malloc(header->payload_size ? header->payload_size : 1);
        if (data && fread(data, 1, header->payload_size, file) != header->payload_size) {
            free(data);
            data = 0;
        }
    }
    fclose(file);
    *size = data ? header->payload_size : 0;
    return data;
}

static void load_slot(Cpu *c, int number)
{
    StateHeader header;
    size_t size;
    uint8_t *data = read_slot(number, &header, &size);
    if (!data) {
        message("Slot %d: empty", number);
        return;
    }
    const char *problem = 0;
    if (header.format != STATE_FORMAT) problem = "made by another version of the program";
    else if (strncmp(header.version, rt_game_version_tag(), sizeof(header.version)) != 0) problem = "another game version";
    else if (payload_hash(data, size) != header.payload_hash) problem = "the file is damaged";
    else if (header.context != context_hash(c)) problem = "made with other game files or drivers";
    if (problem) {
        free(data);
        message("Slot %d: cannot load (%s)", number, problem);
        return;
    }
    StateStream s = {1, data, size, size, 0, 0};
    machine_state(c, &s);
    free(data);
    if (s.error || s.offset != s.size) rt_fatal("slot %d: the snapshot does not match this program", number);
    pc98_after_load();
    display_after_load();
    message("Loaded slot %d (stage %d)", number, header.stage);
}

static void read_settings(void)
{
    settings_read = 1;
    const char *save_at = getenv("RUSTY_STATE_SAVE_AT");
    if (save_at && sscanf(save_at, "%ld:%d", &save_at_frame, &save_at_slot) != 2) save_at_frame = -1;
    const char *load = getenv("RUSTY_STATE_LOAD");
    if (load) load_slot_at_start = atoi(load);
}

void rt_safe_point(Cpu *c, uint16_t address)
{
    (void)address;
    if (!settings_read) read_settings();
    last_safe_frame = pc98_frame();
    if (dos_open_handles()) return;
    if (load_slot_at_start) {
        int number = load_slot_at_start;
        load_slot_at_start = 0;
        load_slot(c, number);
    }
    if (save_at_frame >= 0 && pc98_frame() >= save_at_frame) {
        save_at_frame = -1;
        save_slot(c, save_at_slot);
    }
    if (request == REQUEST_SAVE) save_slot(c, slot);
    if (request == REQUEST_LOAD) load_slot(c, slot);
    request = 0;
}

void state_frame(void)
{
    if (request && pc98_frame() - request_frame > REQUEST_TIMEOUT_FRAMES &&
        pc98_frame() - last_safe_frame > REQUEST_TIMEOUT_FRAMES) {
        request = 0;
        message("Saving and loading work only during play (not in menus or the pause)");
    }
}

void state_request(int load)
{
    request = load ? REQUEST_LOAD : REQUEST_SAVE;
    request_frame = pc98_frame();
}

void state_change_slot(int delta)
{
    slot = (slot - 1 + delta + SLOT_COUNT) % SLOT_COUNT + 1;
    StateHeader header;
    size_t size;
    uint8_t *data = read_slot(slot, &header, &size);
    if (!data) {
        message("Slot %d: empty", slot);
        return;
    }
    free(data);
    time_t saved_at = (time_t)header.saved_at;
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&saved_at));
    message("Slot %d: stage %d, saved %s", slot, header.stage, when);
}
