/* Debug cheats for exploring the game (not part of the original), and a message for the game speed.
 * God mode (G, or RUSTY_GODMODE=1): at every frame boundary, while JO.EXE is loaded, player_health is set to
 * full (6), player_lives to 9 and time_left to full (1F3Fh frames). Addresses come from the naming work
 * (tools/recomp/names/JO.EXE.tsv, data segment 06E7h of the image). A pit or a deadly tile still kills in one step,
 * but no life is lost. The switch is a recorded input event, so replays stay exact.
 * When game_speed_frames (F1 faster, F10 slower: 0-10 vsync frames per game frame) changes, a message shows the speed
 * (10 - game_speed_frames, so 10 is the fastest). */
#include "rt.h"

#include <stdio.h>

#define JO_DATA_SEGMENT 0x06E7
#define PLAYER_HEALTH 0x29E2
#define PLAYER_LIVES 0x29E3
#define TIME_LEFT 0x29EC
#define FULL_HEALTH 6
#define LIVES 9
#define FULL_TIME 0x1F3F
#define GAME_SPEED_FRAMES 0x2A8C
#define SLOWEST_SPEED_FRAMES 10

static int godmode;
static int shown_speed_frames = -1;

void cheats_set_godmode(int on)
{
    if (godmode == on) return;
    godmode = on;
    rt_log("god mode %s", on ? "on" : "off");
    display_message(on ? "God mode on" : "God mode off", 3);
}

static void show_speed_change(Cpu *c, uint16_t data)
{
    int frames = rd16(c, data, GAME_SPEED_FRAMES);
    if (frames > SLOWEST_SPEED_FRAMES) return;
    if (shown_speed_frames >= 0 && frames != shown_speed_frames) {
        char text[64];
        snprintf(text, sizeof(text), "Game speed %d/%d", SLOWEST_SPEED_FRAMES - frames, SLOWEST_SPEED_FRAMES);
        display_message(text, 2);
    }
    shown_speed_frames = frames;
}

void cheats_frame(Cpu *c)
{
    uint16_t load_seg;
    if (!rt_instance_segment("JO.EXE", &load_seg) && !rt_instance_segment("en/JO.EXE", &load_seg)) {
        shown_speed_frames = -1;
        return;
    }
    uint16_t data = (uint16_t)(load_seg + JO_DATA_SEGMENT);
    show_speed_change(c, data);
    if (!godmode) return;
    wr8(c, data, PLAYER_HEALTH, FULL_HEALTH);
    wr8(c, data, PLAYER_LIVES, LIVES);
    wr16(c, data, TIME_LEFT, FULL_TIME);
}

void cheats_snapshot(StateStream *s)
{
    STATE(s, godmode);
}
