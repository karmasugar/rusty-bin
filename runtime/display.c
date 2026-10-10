/* Host display and keyboard. With -DRUSTY_SDL: SDL3 window with the CPU frame (video_render) in an SDL renderer;
 * RUSTY_RENDERER=gpu decodes and scales the frame on the GPU (gpu.c, display filters), with the CPU frame as the
 * fallback when no GPU device can be used. PC-98 key codes
 * come from host keys. Without RUSTY_SDL (tests, headless runs) all functions are empty. RUSTY_SCALE sets the window
 * scale (default 2). Sound goes to an SDL audio stream (the emulation keeps about 70 ms queued, at most 0.2 s;
 * RUSTY_MUTE=1 disables it). F5 saves and F9 loads a snapshot in the current slot, F6/F7 select the slot (state.c),
 * G switches god mode (cheats.c),
 * F11 switches full screen. Gamepads (SDL3 gamepad API, hot plug) press the same PC-98 keys: D-pad and left stick =
 * arrows, A (south) = Z (jump), X (west) = X (whip), Y (north) = Z+X (special weapon, both keys in the same frame),
 * B (east) + left/right = run (the game runs on up+left / up+right), Start = Return, Back = ESC (pause), RB/LB =
 * F1/F10 (game speed). Messages (display_message: missing ROMs, saves, god mode, game speed, gamepads) are drawn over
 * the game frame with the SDL debug font for some seconds (not with RUSTY_RENDERER=gpu, not in frame dumps); a fatal
 * error also opens a message window (display_fatal). */
#include "rt.h"
#include "sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MESSAGES 4
#define MESSAGE_SIZE 256

typedef struct {
    char text[MESSAGE_SIZE];
    int seconds;
    uint64_t until_ms;
} Message;

/* Waiting or shown messages; the time of a message starts when it is first drawn (it can come before the window). */
static Message messages[MAX_MESSAGES];
static int message_count;

void display_message(const char *text, int seconds)
{
    if (message_count == MAX_MESSAGES) {
        memmove(messages, messages + 1, sizeof(messages[0]) * (MAX_MESSAGES - 1));
        message_count--;
    }
    Message *message = &messages[message_count++];
    snprintf(message->text, sizeof(message->text), "%s", text);
    message->seconds = seconds;
    message->until_ms = 0;
}

static uint8_t key_down[128];
static uint8_t key_queue[32];
static int queue_head;
static int queue_tail;

int display_key_down(uint8_t code)
{
    return code < 128 && key_down[code];
}

int display_key_queued(uint8_t *code, int remove)
{
    if (queue_head == queue_tail) return 0;
    *code = key_queue[queue_head];
    if (remove) queue_head = (queue_head + 1) % 32;
    return 1;
}

void display_key_push(uint8_t code)
{
    if ((queue_tail + 1) % 32 != queue_head) {
        key_queue[queue_tail] = code;
        queue_tail = (queue_tail + 1) % 32;
    }
}

void display_key_set(uint8_t code, int down)
{
    if (code >= 128) return;
    if (down && !key_down[code] && (queue_tail + 1) % 32 != queue_head) {
        key_queue[queue_tail] = code;
        queue_tail = (queue_tail + 1) % 32;
    }
    key_down[code] = (uint8_t)down;
}

/* A key held or released without a key press in the queue (after a load: input_after_load). */
void display_key_hold(uint8_t code, int down)
{
    if (code < 128) key_down[code] = (uint8_t)down;
}

/* Snapshot: the PC-98 keyboard (keys down and the key queue). */
void display_snapshot(StateStream *s)
{
    STATE(s, key_down);
    STATE(s, key_queue);
    STATE(s, queue_head);
    STATE(s, queue_tail);
}

#ifdef RUSTY_SDL
#include "gpu.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioStream *audio;
static int gpu_output;

#define PC98_ESC 0x00
#define PC98_RETURN 0x1C
#define PC98_Z 0x29
#define PC98_X 0x2A
#define PC98_UP 0x43
#define PC98_LEFT 0x46
#define PC98_RIGHT 0x48
#define PC98_DOWN 0x4B
#define PC98_F1 0x62
#define PC98_F10 0x6B
#define STICK_THRESHOLD 16000
#define SOURCE_STICK (1u << 31)
#define SOURCE_RUN (1u << 30)

/* The gamepad inputs that hold each PC-98 key (a bit per button, SOURCE_STICK for the left stick): the key goes up
 * when the last one is released, so the D-pad and the stick (or A and Y, both Z) do not cancel each other. */
static uint32_t gamepad_sources[128];
static int run_button;

#define TEXT_COLUMNS 78
#define LINE_HEIGHT 10.0f
#define TEXT_LEFT 8.0f

/* The next line of text (at most TEXT_COLUMNS characters, broken at a space or a newline); returns its length and
 * sets *next to the start of the following line. */
static int next_line(const char *text, const char **next)
{
    int length = 0, space = -1;
    while (text[length] && text[length] != '\n' && length < TEXT_COLUMNS) {
        if (text[length] == ' ') space = length;
        length++;
    }
    if (text[length] && text[length] != '\n' && space > 0) length = space;
    *next = text + length + (text[length] == ' ' || text[length] == '\n');
    return length;
}

static void draw_messages(void)
{
    uint64_t now = SDL_GetTicks();
    int kept = 0;
    for (int i = 0; i < message_count; i++) {
        if (!messages[i].until_ms) messages[i].until_ms = now + (uint64_t)messages[i].seconds * 1000;
        if (messages[i].until_ms > now) messages[kept++] = messages[i];
    }
    message_count = kept;
    float y = 6.0f;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < message_count; i++) {
        const char *text = messages[i].text;
        while (*text) {
            const char *next;
            char line[TEXT_COLUMNS + 1];
            int length = next_line(text, &next);
            memcpy(line, text, (size_t)length);
            line[length] = 0;
            SDL_FRect box = {TEXT_LEFT - 4.0f, y - 1.0f, (float)length * 8.0f + 8.0f, LINE_HEIGHT};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
            SDL_RenderFillRect(renderer, &box);
            SDL_SetRenderDrawColor(renderer, 255, 255, 160, 255);
            SDL_RenderDebugText(renderer, TEXT_LEFT, y, line);
            y += LINE_HEIGHT;
            text = next;
        }
        y += 4.0f;
    }
}

void display_after_load(void)
{
    if (audio) SDL_ClearAudioStream(audio);
}

void display_fatal(const char *text)
{
    if (getenv("RUSTY_HEADLESS") || getenv("RUSTY_FAST")) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rusty", text, window);
}

static int pc98_code(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_UP: return 0x43;
    case SDL_SCANCODE_DOWN: return 0x4B;
    case SDL_SCANCODE_LEFT: return 0x46;
    case SDL_SCANCODE_RIGHT: return 0x48;
    case SDL_SCANCODE_KP_8: return 0x43;
    case SDL_SCANCODE_KP_2: return 0x4B;
    case SDL_SCANCODE_KP_4: return 0x46;
    case SDL_SCANCODE_KP_6: return 0x48;
    case SDL_SCANCODE_Z: return 0x29;
    case SDL_SCANCODE_X: return 0x2A;
    case SDL_SCANCODE_SPACE: return 0x34;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_ESCAPE: return 0x00;
    case SDL_SCANCODE_F1: return 0x62;
    case SDL_SCANCODE_F2: return 0x63;
    case SDL_SCANCODE_F10: return 0x6B;
    default: return -1;
    }
}

double display_audio_queued_seconds(void)
{
    if (!audio) return -1;
    return (double)SDL_GetAudioStreamQueued(audio) / (4.0 * sound_output_rate());
}

void display_audio(const int16_t *samples, int frames)
{
    if (!audio) return;
    int limit = (int)(sound_output_rate() * 4 / 5);
    if (SDL_GetAudioStreamQueued(audio) > limit) return;
    SDL_PutAudioStreamData(audio, samples, frames * 4);
}

static void open_audio(void)
{
    if (getenv("RUSTY_MUTE") || !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)sound_output_rate()};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, 0, 0);
    if (!audio) {
        rt_log("SDL audio: %s (no sound)", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(audio);
    sound_set_audio_output(1);
    rt_log("SDL audio: %d Hz stereo", spec.freq);
}

static int open_cpu_renderer(void)
{
    renderer = SDL_CreateRenderer(window, 0);
    if (!renderer) {
        rt_log("SDL renderer: %s", SDL_GetError());
        return 0;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderLogicalPresentation(renderer, 640, 400, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 640, 400);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    rt_log("display: CPU frame with SDL renderer %s", SDL_GetRendererName(renderer));
    return 1;
}

int display_init(void)
{
    if (getenv("RUSTY_HEADLESS")) {
        if (!getenv("RUSTY_GPU_CHECK") || !SDL_Init(SDL_INIT_VIDEO)) return 0;
        gpu_output = gpu_init(0);
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        rt_log("SDL init failed: %s (headless)", SDL_GetError());
        return 0;
    }
    int scale = getenv("RUSTY_SCALE") ? atoi(getenv("RUSTY_SCALE")) : 2;
    window = SDL_CreateWindow("Rusty (recompiled)", 640 * scale, 400 * scale, SDL_WINDOW_RESIZABLE);
    if (!window) {
        rt_log("SDL window: %s (headless)", SDL_GetError());
        return 0;
    }
    const char *choice = getenv("RUSTY_RENDERER");
    if (choice && strcmp(choice, "gpu") == 0) gpu_output = gpu_init(window);
    if (!gpu_output && !open_cpu_renderer()) return 0;
    rt_log("display: SDL window %dx%d", 640 * scale, 400 * scale);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) rt_log("SDL gamepad: %s (no gamepads)", SDL_GetError());
    open_audio();
    return 1;
}

static void gamepad_hold(uint8_t code, uint32_t source, int down)
{
    uint32_t before = gamepad_sources[code];
    uint32_t after = down ? before | source : before & ~source;
    gamepad_sources[code] = after;
    if (!before != !after) pc98_host_key(code, after != 0);
}

static int gamepad_button_keys(SDL_GamepadButton button, uint8_t *keys)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: keys[0] = PC98_UP; return 1;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: keys[0] = PC98_DOWN; return 1;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: keys[0] = PC98_LEFT; return 1;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: keys[0] = PC98_RIGHT; return 1;
    case SDL_GAMEPAD_BUTTON_SOUTH: keys[0] = PC98_Z; return 1;
    case SDL_GAMEPAD_BUTTON_WEST: keys[0] = PC98_X; return 1;
    case SDL_GAMEPAD_BUTTON_NORTH: keys[0] = PC98_Z; keys[1] = PC98_X; return 2;
    case SDL_GAMEPAD_BUTTON_START: keys[0] = PC98_RETURN; return 1;
    case SDL_GAMEPAD_BUTTON_BACK: keys[0] = PC98_ESC; return 1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: keys[0] = PC98_F1; return 1;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: keys[0] = PC98_F10; return 1;
    default: return 0;
    }
}

/* Run: the game runs on up+left / up+right, so B with a horizontal direction also holds up. */
static void update_run(void)
{
    int horizontal = gamepad_sources[PC98_LEFT] || gamepad_sources[PC98_RIGHT];
    gamepad_hold(PC98_UP, SOURCE_RUN, run_button && horizontal);
}

static void handle_gamepad_button(const SDL_GamepadButtonEvent *event)
{
    if (event->button == SDL_GAMEPAD_BUTTON_EAST) run_button = event->down;
    uint8_t keys[2];
    int count = gamepad_button_keys((SDL_GamepadButton)event->button, keys);
    for (int i = 0; i < count; i++) gamepad_hold(keys[i], 1u << event->button, event->down);
    update_run();
}

static void handle_gamepad_axis(const SDL_GamepadAxisEvent *event)
{
    if (event->axis == SDL_GAMEPAD_AXIS_LEFTX) {
        gamepad_hold(PC98_LEFT, SOURCE_STICK, event->value < -STICK_THRESHOLD);
        gamepad_hold(PC98_RIGHT, SOURCE_STICK, event->value > STICK_THRESHOLD);
    } else if (event->axis == SDL_GAMEPAD_AXIS_LEFTY) {
        gamepad_hold(PC98_UP, SOURCE_STICK, event->value < -STICK_THRESHOLD);
        gamepad_hold(PC98_DOWN, SOURCE_STICK, event->value > STICK_THRESHOLD);
    }
    update_run();
}

static void handle_gamepad_device(const SDL_GamepadDeviceEvent *event)
{
    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad *gamepad = SDL_OpenGamepad(event->which);
        rt_log("gamepad: %s", gamepad ? SDL_GetGamepadName(gamepad) : SDL_GetError());
        if (gamepad) {
            char text[MESSAGE_SIZE];
            snprintf(text, sizeof(text), "Gamepad connected: %s", SDL_GetGamepadName(gamepad));
            display_message(text, 3);
        }
        return;
    }
    SDL_Gamepad *gamepad = SDL_GetGamepadFromID(event->which);
    if (gamepad) SDL_CloseGamepad(gamepad);
    run_button = 0;
    for (int code = 0; code < 128; code++) {
        if (gamepad_sources[code]) gamepad_hold((uint8_t)code, gamepad_sources[code], 0);
    }
    rt_log("gamepad removed");
    display_message("Gamepad removed", 3);
}

static void handle_key(const SDL_KeyboardEvent *key)
{
    if (key->down && !key->repeat) {
        switch (key->scancode) {
        case SDL_SCANCODE_F5: state_request(0); return;
        case SDL_SCANCODE_F9: state_request(1); return;
        case SDL_SCANCODE_F6: state_change_slot(-1); return;
        case SDL_SCANCODE_F7: state_change_slot(1); return;
        case SDL_SCANCODE_G: pc98_toggle_godmode(); return;
        case SDL_SCANCODE_F11:
            SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            return;
        default: break;
        }
    }
    int code = pc98_code(key->scancode);
    if (code >= 0) pc98_host_key((uint8_t)code, key->down);
}

void display_frame(void)
{
    if (!window) {
        if (gpu_output) gpu_frame();
        return;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            rt_log("window closed: stop");
            exit(0);
        }
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: handle_key(&event.key); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: handle_gamepad_button(&event.gbutton); break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: handle_gamepad_axis(&event.gaxis); break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED: handle_gamepad_device(&event.gdevice); break;
        default: break;
        }
    }
    if (gpu_output) {
        gpu_frame();
        return;
    }
    SDL_UpdateTexture(texture, 0, video_render(), 640 * 3);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, 0, 0);
    draw_messages();
    SDL_RenderPresent(renderer);
}

#else

double display_audio_queued_seconds(void)
{
    return -1;
}

void display_audio(const int16_t *samples, int frames)
{
    (void)samples;
    (void)frames;
}

int display_init(void)
{
    return 0;
}

void display_fatal(const char *text)
{
    (void)text;
}

void display_after_load(void)
{
}

void display_frame(void)
{
}

#endif
