/* Key input at frame boundaries, recording and replay (a dev tool; the saves for players are snapshots, state.c).
 * Every key change (host keys, RUSTY_KEYS, RUSTY_ENTER_AT, replayed keys) takes effect at a frame boundary and is
 * recorded with its frame number. The emulated machine is deterministic (time = instruction count), so replaying
 * the same key changes from the start reaches the same state. RUSTY_SAVE_AT=<frame> writes the recording up to that
 * frame to RUSTY_SAVE (default save.rec in the user folder, build/recomp in dev builds); RUSTY_LOAD=<file> replays it
 * at full speed without a window and continues live from the saved frame. The recording keeps working
 * after a rebuild as long as the translated code behaves the same. Runtime events (god mode, G) are recorded the
 * same way. The save also stores RUSTY_STAGE and the game's continue file (GAMEDATA.CFG) as they were at the
 * start; loading restores them. */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


typedef struct {
    long frame;
    uint8_t code;
    uint8_t down;
} KeyChange;

static KeyChange *changes;
static size_t change_count;
static size_t change_capacity;
static KeyChange *scripted;
static size_t scripted_count;
static size_t scripted_capacity;
static KeyChange *replay;
static size_t replay_count;
static size_t replay_next;
static long replay_end = -1;
static long long base_time;
static int stage;

/* The game's continue file as it was when the session started: a save restores it, so the replay reads the same
 * data (the game writes it at the continue screen and reads it for "Continue"). */
#define CONTINUE_FILE "GAMEDATA.CFG"
#define CONTINUE_MAX 64
static uint8_t continue_data[CONTINUE_MAX];
static long continue_size = -1;

static void record(long frame, uint8_t code, uint8_t down)
{
    if (change_count == change_capacity) {
        change_capacity = change_capacity ? change_capacity * 2 : 256;
        changes = realloc(changes, change_capacity * sizeof(KeyChange));
        if (!changes) rt_fatal("out of memory for the key recording");
    }
    changes[change_count++] = (KeyChange){frame, code, down};
}

/* Codes from F0h are runtime events, not PC-98 keys: F0h = god mode (down = on/off). */
#define EVENT_GODMODE 0xF0

static void apply(long frame, uint8_t code, uint8_t down)
{
    record(frame, code, down);
    if (code == EVENT_GODMODE) {
        cheats_set_godmode(down);
        return;
    }
    display_key_set(code, down);
}

static int godmode_on;
static int snapshot_loaded;

void input_host_special(long frame, uint8_t code)
{
    if (input_replaying(frame) || code != EVENT_GODMODE) return;
    godmode_on = !godmode_on;
    apply(frame, EVENT_GODMODE, (uint8_t)godmode_on);
}

static void add_scripted(long frame, uint8_t code, long length)
{
    if (scripted_count + 2 > scripted_capacity) {
        scripted_capacity = scripted_capacity ? scripted_capacity * 2 : 64;
        scripted = realloc(scripted, scripted_capacity * sizeof(KeyChange));
        if (!scripted) rt_fatal("out of memory for the scripted keys");
    }
    scripted[scripted_count++] = (KeyChange){frame, code, 1};
    scripted[scripted_count++] = (KeyChange){frame + length, code, 0};
}

static void parse_scripted(const char *text)
{
    while (text && *text) {
        char *end;
        long frame = strtol(text, &end, 10);
        if (*end != ':') break;
        long code = strtol(end + 1, &end, 16);
        long length = 10;
        if (*end == ':') length = strtol(end + 1, &end, 10);
        add_scripted(frame, (uint8_t)code, length);
        text = *end == ',' ? end + 1 : 0;
    }
}

static void load_recording(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) rt_fatal("cannot open the recording %s", path);
    int version = 0;
    if (fscanf(file, "rusty-record %d base %lld end %ld", &version, &base_time, &replay_end) != 3 || version < 1 ||
        version > 2) {
        rt_fatal("%s is not a rusty recording", path);
    }
    if (version == 2) {
        char hex[2 * CONTINUE_MAX + 2];
        if (fscanf(file, " stage %d file " CONTINUE_FILE " %129s", &stage, hex) != 2) {
            rt_fatal("%s: bad version 2 header", path);
        }
        continue_size = -1;
        if (strcmp(hex, "-") != 0) {
            continue_size = 0;
            for (const char *p = hex; p[0] && p[1] && continue_size < CONTINUE_MAX; p += 2) {
                unsigned value;
                sscanf(p, "%2x", &value);
                continue_data[continue_size++] = (uint8_t)value;
            }
        }
        dos_write_save_file(CONTINUE_FILE, continue_data, continue_size);
    }
    long frame;
    unsigned code, down;
    while (fscanf(file, "%ld %x %u", &frame, &code, &down) == 3) {
        replay = realloc(replay, (replay_count + 1) * sizeof(KeyChange));
        if (!replay) rt_fatal("out of memory for the recording");
        replay[replay_count++] = (KeyChange){frame, (uint8_t)code, (uint8_t)down};
    }
    fclose(file);
    rt_log("replay %s: %zu key changes up to frame %ld", path, replay_count, replay_end);
}

/* RUSTY_STAGE=n: write the continue file for stage n into the save folder, encoded as progress_save does (the key
 * text with the stage index added or subtracted per byte: + - - + + - - + ...). The original file holds stage 1
 * (index 0), so its bytes are the key text. "Continue" in the title menu then starts at stage n. */
static void write_continue_stage(int number)
{
    static const int8_t signs[16] = {1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1};
    uint8_t key[CONTINUE_MAX];
    long size = dos_read_original_file(CONTINUE_FILE, key, sizeof(key));
    if (size != 16) rt_fatal("RUSTY_STAGE: the original %s is missing or has another size", CONTINUE_FILE);
    for (int i = 0; i < 16; i++) continue_data[i] = (uint8_t)(key[i] + signs[i] * (number - 1));
    continue_size = 16;
    dos_write_save_file(CONTINUE_FILE, continue_data, continue_size);
    rt_log("RUSTY_STAGE: continue file for stage %d written; choose Continue in the title menu", number);
}

void input_init(void)
{
    base_time = (long long)time(0);
    const char *stage_setting = getenv("RUSTY_STAGE");
    stage = stage_setting ? atoi(stage_setting) : 0;
    const char *load = getenv("RUSTY_LOAD");
    if (load && *load) {
        load_recording(load);
    } else if (stage > 0) {
        write_continue_stage(stage);
    } else {
        continue_size = dos_read_game_file(CONTINUE_FILE, continue_data, CONTINUE_MAX);
    }
    parse_scripted(getenv("RUSTY_KEYS"));
    const char *enter = getenv("RUSTY_ENTER_AT");
    if (enter && atol(enter) >= 0) add_scripted(atol(enter), 0x1C, 10);
    const char *god = getenv("RUSTY_GODMODE");
    if (god && atoi(god) && !load) {
        godmode_on = 1;
        add_scripted(1, EVENT_GODMODE, 0x7FFFFFFF);
    }
}

int input_replaying(long frame)
{
    return frame < replay_end;
}

void input_frame(long previous, long frame)
{
    while (replay_next < replay_count && replay[replay_next].frame <= frame) {
        KeyChange *change = &replay[replay_next++];
        apply(change->frame, change->code, change->down);
    }
    for (size_t i = 0; i < scripted_count; i++) {
        if (scripted[i].frame > previous && scripted[i].frame <= frame) apply(frame, scripted[i].code, scripted[i].down);
    }
}

static uint8_t host_held[128];

void input_host_key(long frame, uint8_t code, int down)
{
    if (input_replaying(frame)) return;
    if (code < 128) host_held[code] = (uint8_t)(down != 0);
    apply(frame, code, (uint8_t)(down != 0));
}

static int scripted_held(uint8_t code, long frame)
{
    for (size_t i = 0; i + 1 < scripted_count; i += 2) {
        if (scripted[i].code == code && scripted[i].frame <= frame && scripted[i + 1].frame > frame) return 1;
    }
    return 0;
}

/* After a snapshot load the keys down are the keys held now (host and scripted keys), not the saved ones: a saved
 * key that is not held any more would never get its release. */
void input_after_load(long frame)
{
    for (uint8_t code = 0; code < 128; code++) display_key_hold(code, host_held[code] || scripted_held(code, frame));
}

long long input_base_time(void)
{
    return base_time;
}

int input_stage(void)
{
    return stage;
}

void input_save(long frame)
{
    const char *path = input_save_path();
    if (snapshot_loaded) {
        rt_log("no replay save after a snapshot load: the recording does not start at the program start");
        return;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        rt_log("cannot write the save %s", path);
        display_message("The session cannot be saved: the save folder cannot be written.", 5);
        return;
    }
    fprintf(file, "rusty-record 2 base %lld end %ld stage %d\nfile " CONTINUE_FILE " ", base_time, frame, stage);
    if (continue_size < 0) fprintf(file, "-");
    for (long i = 0; i < continue_size; i++) fprintf(file, "%02x", continue_data[i]);
    fprintf(file, "\n");
    for (size_t i = 0; i < change_count; i++) {
        fprintf(file, "%ld %02x %u\n", changes[i].frame, changes[i].code, changes[i].down);
    }
    fclose(file);
    rt_log("saved: %s (frame %ld, %zu key changes)", path, frame, change_count);
    display_message("Session saved (F9 loads it)", 3);
}

/* RUSTY_SAVE, else save.rec in the user folder (dos_user_dir). */
const char *input_save_path(void)
{
    static char path[512];
    const char *setting = getenv("RUSTY_SAVE");
    if (setting && *setting) return setting;
    if (!path[0]) snprintf(path, sizeof(path), "%s/save.rec", dos_user_dir());
    return path;
}

/* Snapshot: the DOS clock base and god mode. A load ends the input recording (a replay save starts at the program
 * start, a snapshot does not). */
void input_snapshot(StateStream *s)
{
    STATE(s, base_time);
    STATE(s, godmode_on);
    if (!state_loading(s)) return;
    change_count = 0;
    snapshot_loaded = 1;
}
