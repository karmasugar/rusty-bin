/* PC-98 hardware model (prototype).
 * - Time: emulated nanoseconds = executed instructions (counted by the generated code) / RUSTY_CYCLES instructions
 *   per millisecond (default 12000, as the DOSBox-X configuration) + idle time of emulated BIOS waits. Events
 *   (vsync, PIT, OPNA timers, interrupts) are checked in rt_poll (backward jumps, GDC status reads).
 *   RUSTY_FAST=1 runs as fast as possible; otherwise the time is held to the wall clock: the runtime sleeps when it
 *   is ahead; when it is far behind, the wall clock reference moves (emulated time never jumps, so replays stay exact).
 * - Vsync: 56.42 Hz, GDC status bits 20h (vblank) and 40h (hblank), IRQ 2 (INT 0Ah) re-armed with OUT 64h.
 * - PIC: master (00h/02h) and slave (08h/0Ah), masks, in-service bits, non-specific and specific EOI. IRQ n goes to
 *   INT 08h+n (master) or INT 10h+n-8 (slave) when IF is set; interrupts are delivered by rt_poll.
 * - PIT (71h/77h) channel 0, modes 0 and 2/3, clock 2.4576 MHz (system port 42h = 94h): IRQ 0.
 * - EGC (RUSTY_EGC=0 for a machine without it): BIOS work byte 0000:054D bit 6, then JO.EXE loads GRPEGC.COM.
 * - Sound (RUSTY_SOUND = 86 / opna / 26k / none): port A460h, OPNA at 188h-18Eh (sound.cpp, IRQ 12) for 86/opna;
 *   26k and none only answer the detection. System port 31h and the BIOS bytes at FD80:0002.
 * - Keys (input.c): host keys, RUSTY_KEYS="frame:code[:frames],..." (PC-98 key code in hex, default 10 frames),
 *   RUSTY_ENTER_AT=<frame> (RETURN), and the replay of a recording (RUSTY_LOAD, F5 saves, F9 loads,
 *   RUSTY_SAVE_AT=<frame> saves without a window). RUSTY_MAX_FRAMES stops the program. RUSTY_TRACE_IRQ=1 logs the interrupt counts at exit. */
#include "rt.h"
#include "sound.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_RATE 56.4206
#define LINES_PER_FRAME 440
#define PIT_CLOCK 2457600
#define NEVER INT64_MAX
#define SOUND_IRQ 12

enum SoundBoard { BOARD_NONE, BOARD_26K, BOARD_OPNA, BOARD_86 };

static enum SoundBoard board = BOARD_86;
static uint8_t board86_mask;
static uint8_t opn_address[2];
static uint8_t dip_switch_31 = 0x80;
static uint8_t master_mask = 0xFF;
static uint8_t slave_mask = 0xFF;
static uint16_t irq_pending;
static uint16_t irq_in_service;
static unsigned sound_edges;
static int vsync_armed;
static long frames_delivered;
static long last_frame;
static int fast_mode;
static long cycles = 12000;
static int64_t idle_ns;
static double ns_per_instruction;
uint64_t rt_next_check_icount;
int rt_interrupt_waiting;
static long poll_count;
static int64_t now_ns;
static int64_t frame_ns;
static int64_t wall_start_ns;
static int live_window;
static long save_at = -1;
static long max_frames = -1;
static long irq_counts[16];
static int64_t pit_due_ns = -1;
static long late_counts[6];
static int64_t late_max;

typedef struct {
    int mode;
    int access;
    int low_next;
    uint16_t reload;
    int64_t period_ns;
    int64_t next_ns;
    int read_low_next;
    uint16_t latched;
} PitChannel;

static PitChannel pit;

static int64_t wall_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static long current_frame(void)
{
    return (long)(now_ns / frame_ns);
}

long pc98_frame(void)
{
    return frames_delivered;
}

int pc98_key_pressed(uint8_t code)
{
    return display_key_down(code);
}

void pc98_host_key(uint8_t code, int down)
{
    input_host_key(current_frame(), code, down);
}

void pc98_toggle_godmode(void)
{
    input_host_special(current_frame(), 0xF0);
}

/* Time spent in emulated system code (DOS calls) that the real machine would run as instructions. */
void pc98_charge_us(Cpu *c, double microseconds)
{
    c->icount += (uint64_t)(microseconds * (double)cycles / 1000.0);
}

double pc98_time_ms(void)
{
    return (double)now_ns / 1e6;
}

long long pc98_clock_seconds(void)
{
    return input_base_time() + now_ns / 1000000000;
}

static int opna_enabled(void)
{
    return board == BOARD_OPNA || (board == BOARD_86 && (board86_mask & 1));
}

static int chip_emulated(void)
{
    return board == BOARD_OPNA || board == BOARD_86;
}

static void report_interrupts(void)
{
    for (int irq = 0; irq < 16; irq++) {
        if (irq_counts[irq]) rt_log("IRQ %d: %ld interrupts", irq, irq_counts[irq]);
    }
    rt_log("frames: %ld", frames_delivered);
    rt_log("IRQ 0 lateness: <5us %ld, <20us %ld, <50us %ld, <100us %ld, <500us %ld, more %ld, max %lld us", late_counts[0],
           late_counts[1], late_counts[2], late_counts[3], late_counts[4], late_counts[5], (long long)(late_max / 1000));
    rt_log("PIC: pending %04x, in service %04x, masks %02x/%02x", irq_pending, irq_in_service, master_mask,
           slave_mask);
}

void pc98_init(Cpu *c)
{
    const char *setting = getenv("RUSTY_SOUND");
    if (setting) {
        if (strcmp(setting, "none") == 0) board = BOARD_NONE;
        else if (strcmp(setting, "26k") == 0) board = BOARD_26K;
        else if (strcmp(setting, "opna") == 0) board = BOARD_OPNA;
        else board = BOARD_86;
    }
    fast_mode = getenv("RUSTY_FAST") != 0;
    if (getenv("RUSTY_CYCLES")) cycles = atol(getenv("RUSTY_CYCLES"));
    if (cycles < 1) cycles = 1;
    frame_ns = (int64_t)(1e9 / FRAME_RATE);
    ns_per_instruction = 1000000.0 / (double)cycles;
    if (getenv("RUSTY_MAX_FRAMES")) max_frames = atol(getenv("RUSTY_MAX_FRAMES"));
    input_init();
    if (getenv("RUSTY_SAVE_AT")) save_at = atol(getenv("RUSTY_SAVE_AT"));
    live_window = !fast_mode;
    if (input_replaying(0)) fast_mode = 1;
    pit.next_ns = NEVER;
    wall_start_ns = wall_ns();
    if (getenv("RUSTY_TRACE_IRQ")) atexit(report_interrupts);
    video_init(c->mem);
    if (chip_emulated()) sound_init();
    if (!fast_mode || getenv("RUSTY_GPU_CHECK")) display_init();
    const char *egc = getenv("RUSTY_EGC");
    int egc_present = !egc || atoi(egc) != 0;
    video_set_egc_available(egc_present);
    if (egc_present) wr8(c, 0, 0x054D, (uint8_t)(rd8(c, 0, 0x054D) | 0x40));
    wr16(c, 0xFD80, 0x0002, 0x2A27);
    wr8(c, 0xFD80, 0x0004, 0x06);
    rt_log("pc98: sound board %s, %ld cycles/ms, %s time", setting ? setting : "86", cycles,
           fast_mode ? "fast" : "real");
}

static int irq_rank(int irq)
{
    return irq < 8 ? irq * 16 : 7 * 16 + (irq - 8);
}

static int irq_masked(int irq)
{
    if (irq < 8) return (master_mask >> irq) & 1;
    return ((slave_mask >> (irq - 8)) & 1) || (master_mask & 0x80);
}

static void update_interrupt_waiting(void)
{
    uint16_t masked = (uint16_t)(master_mask | (slave_mask << 8));
    if (master_mask & 0x80) masked |= 0xFF00;
    rt_interrupt_waiting = (irq_pending & ~masked) != 0;
}

static void raise_irq(int irq)
{
    irq_pending |= (uint16_t)(1 << irq);
    update_interrupt_waiting();
}

static void end_of_interrupt(int slave, uint8_t command)
{
    int first = slave ? 8 : 0;
    if ((command & 0xE0) == 0x60) {
        irq_in_service &= (uint16_t)~(1 << (first + (command & 7)));
        return;
    }
    if ((command & 0xE0) != 0x20) return;
    int best = -1;
    for (int irq = first; irq < first + 8; irq++) {
        if (((irq_in_service >> irq) & 1) && (best < 0 || irq_rank(irq) < irq_rank(best))) best = irq;
    }
    if (best >= 0) irq_in_service &= (uint16_t)~(1 << best);
}

void pc98_end_of_interrupt(int slave)
{
    if (slave) end_of_interrupt(1, 0x20);
    end_of_interrupt(0, 0x20);
}

static void deliver_interrupts(Cpu *c)
{
    while (irq_pending && (c->flags & F_IF)) {
        int chosen = -1;
        for (int irq = 0; irq < 16; irq++) {
            if (!((irq_pending >> irq) & 1) || irq_masked(irq)) continue;
            if (chosen < 0 || irq_rank(irq) < irq_rank(chosen)) chosen = irq;
        }
        if (chosen < 0) return;
        for (int irq = 0; irq < 16; irq++) {
            if (((irq_in_service >> irq) & 1) && irq_rank(irq) <= irq_rank(chosen)) return;
        }
        irq_pending &= (uint16_t)~(1 << chosen);
        irq_in_service |= (uint16_t)(1 << chosen);
        if (chosen >= 8) irq_in_service |= 0x80;
        if (chosen == 2) vsync_armed = 0;
        irq_counts[chosen]++;
        if (chosen == 0 && pit_due_ns >= 0) {
            int64_t late = now_ns - pit_due_ns;
            int bucket = late < 5000 ? 0 : late < 20000 ? 1 : late < 50000 ? 2 : late < 100000 ? 3 : late < 500000 ? 4 : 5;
            late_counts[bucket]++;
            if (late > late_max) late_max = late;
            pit_due_ns = -1;
        }
        rt_call_interrupt(c, (uint8_t)(chosen < 8 ? 0x08 + chosen : 0x10 + chosen - 8));
    }
}

static void run_pit(void)
{
    if (pit.next_ns > now_ns) return;
    if (pit_due_ns < 0) pit_due_ns = pit.next_ns;
    raise_irq(0);
    if (pit.mode == 0) {
        pit.next_ns = NEVER;
        return;
    }
    pit.next_ns += pit.period_ns;
    if (pit.next_ns <= now_ns) pit.next_ns = now_ns + pit.period_ns;
}

static void update_time(Cpu *c)
{
    int64_t t = (int64_t)((double)c->icount * ns_per_instruction) + idle_ns;
    if (t > now_ns) now_ns = t;
}

/* rt_poll does the full event check only when the instruction count reaches the next event (frame, PIT, OPNA
 * timer, at most 1 ms later), or when an interrupt is pending. */
static void plan_next_check(Cpu *c)
{
    int64_t next = (current_frame() + 1) * frame_ns;
    if (pit.next_ns < next) next = pit.next_ns;
    if (chip_emulated()) {
        int64_t timer = sound_next_event();
        if (timer < next) next = timer;
    }
    if (now_ns + 1000000 < next) next = now_ns + 1000000;
    double instructions = (double)(next - idle_ns) / ns_per_instruction;
    rt_next_check_icount = instructions > (double)c->icount ? (uint64_t)instructions : c->icount;
}

/* With audio the emulation follows the audio queue: it runs ahead until about 70 ms of sound are queued, so short
 * host delays (vsync of the window, sleeps) never empty the queue, and the audio clock sets the speed. */
#define AUDIO_RESERVE_SECONDS 0.07

static void keep_to_wall_clock(void)
{
    if (fast_mode) return;
    double queued = display_audio_queued_seconds();
    if (queued >= 0) {
        while (queued > AUDIO_RESERVE_SECONDS) {
            usleep(2000);
            queued = display_audio_queued_seconds();
        }
        wall_start_ns = wall_ns() - now_ns;
        return;
    }
    int64_t wall = wall_ns() - wall_start_ns;
    if (now_ns > wall + 2000000) {
        usleep((useconds_t)((now_ns - wall) / 1000));
    } else if (now_ns < wall - 100000000) {
        wall_start_ns += wall - now_ns;
    }
}

/* The replay reached the saved frame: continue in the window, in real time from here. */
static void go_live(void)
{
    fast_mode = 0;
    wall_start_ns = wall_ns() - now_ns;
    display_init();
    rt_log("replay done at frame %ld: live", current_frame());
}

void pc98_idle(Cpu *c)
{
    idle_ns += 10000;
    rt_next_check_icount = 0;
    rt_poll(c);
}

/* FNV-1a 64 of the registers and the memory: with the instruction count it shows that two runs (an older build, the
 * loop skip on and off) reached the same state, not only the same pictures. */
static uint64_t state_fingerprint(const Cpu *c)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    const uint8_t *registers = (const uint8_t *)c;
    for (size_t i = 0; i < offsetof(Cpu, ret_cs) + sizeof(uint16_t); i++) hash = (hash ^ registers[i]) * 0x100000001b3ull;
    for (uint32_t i = 0; i < RT_MEMORY_SIZE; i++) hash = (hash ^ c->mem[i]) * 0x100000001b3ull;
    return hash;
}

void rt_poll_events(Cpu *c)
{
    rt_changes++;
    poll_count++;
    update_time(c);
    keep_to_wall_clock();
    run_pit();
    if (chip_emulated()) {
        unsigned edges = sound_update(now_ns);
        if (edges != sound_edges) raise_irq(SOUND_IRQ);
        sound_edges = edges;
    }
    long frame = current_frame();
    if (frame != last_frame) {
        long previous = last_frame;
        last_frame = frame;
        frames_delivered++;
        if (chip_emulated()) sound_flush();
        video_frame(frames_delivered);
        display_frame();
        if (max_frames >= 0 && frames_delivered >= max_frames) {
            rt_log("frame limit %ld reached: stop (instructions %llu, state %016llx)", max_frames,
                   (unsigned long long)c->icount, (unsigned long long)state_fingerprint(c));
            exit(0);
        }
        if (vsync_armed) raise_irq(2);
        input_frame(previous, frame);
        cheats_frame(c);
        state_frame();
        if (save_at > previous && save_at <= frame) input_save(frame);
        if (fast_mode && live_window && !input_replaying(frame)) go_live();
        if (fast_mode && live_window && frame % 2000 == 0) rt_log("replay: frame %ld", frame);
    }
    if (irq_pending) deliver_interrupts(c);
    plan_next_check(c);
    update_interrupt_waiting();
}

static uint8_t gdc_status(void)
{
    double phase = (double)(now_ns % frame_ns) / (double)frame_ns;
    double line_phase = phase * LINES_PER_FRAME;
    int hblank = line_phase - (double)(long)line_phase < 0.25;
    return (uint8_t)(0x04 | (phase < 0.06 ? 0x20 : 0x00) | (hblank ? 0x40 : 0x00));
}

/* Next time the GDC status can change: vblank start/end or an hblank edge. */
static int64_t next_status_change(void)
{
    int64_t frame_start = now_ns - now_ns % frame_ns;
    int64_t in_frame = now_ns - frame_start;
    int64_t vblank_end = (int64_t)(frame_ns * 0.06);
    int64_t next = in_frame < vblank_end ? vblank_end : frame_ns;
    double line_ns = (double)frame_ns / LINES_PER_FRAME;
    double line = (double)in_frame / line_ns;
    double line_start = (double)(long)line;
    double edge = line - line_start < 0.25 ? line_start + 0.25 : line_start + 1.0;
    int64_t hblank_edge = (int64_t)(edge * line_ns) + 1;
    if (hblank_edge < next) next = hblank_edge;
    return frame_start + next;
}

/* A loop that only reads the GDC status (waiting for vsync or hblank; a read within 32 instruction units, port
 * time included): move the time to the next status change or
 * event instead of running the loop pass by pass. The result is the same; it only costs less. */
static uint64_t gdc_last_read_icount;
static uint8_t gdc_last_value;

static uint8_t read_gdc_status(Cpu *c)
{
    rt_poll(c);
    update_time(c);
    uint8_t value = gdc_status();
    if (c->icount - gdc_last_read_icount <= 32 && value == gdc_last_value) {
        int64_t target = next_status_change();
        int64_t limit = (int64_t)((double)rt_next_check_icount * ns_per_instruction) + idle_ns;
        if (limit > now_ns && limit < target) target = limit;
        if (target > now_ns) {
            idle_ns += target - now_ns;
            now_ns = target;
            rt_next_check_icount = 0;
            rt_poll(c);
            value = gdc_status();
        }
    }
    gdc_last_read_icount = c->icount;
    gdc_last_value = value;
    return value;
}

static uint8_t opn_read(int chip_port, int data)
{
    if (board == BOARD_NONE) return 0xFF;
    if (chip_port == 1 && !opna_enabled()) return 0xFF;
    if (chip_port == 0 && data && opn_address[0] == 0xFF) return opna_enabled() ? 0x01 : 0x00;
    if (chip_emulated()) return sound_read(chip_port * 2 + data, now_ns);
    return 0x00;
}

static void opn_write(int chip_port, int data, uint8_t value)
{
    if (!data) opn_address[chip_port] = value;
    if (board == BOARD_NONE || (chip_port == 1 && !opna_enabled())) return;
    if (chip_emulated()) sound_write(chip_port * 2 + data, value, now_ns);
    rt_next_check_icount = 0;
}


static uint16_t pit_count_left(void)
{
    if (pit.next_ns == NEVER || !pit.period_ns) return pit.reload;
    return (uint16_t)((pit.next_ns - now_ns) * (pit.reload ? pit.reload : 0x10000) / pit.period_ns);
}

static void pit_control(uint8_t value)
{
    if ((value >> 6) != 0) return;
    int access = (value >> 4) & 3;
    pit.read_low_next = 1;
    if (access == 0) {
        pit.latched = pit_count_left();
        return;
    }
    pit.access = access;
    pit.mode = (value >> 1) & 7;
    pit.low_next = 1;
    pit.next_ns = NEVER;
}

static void pit_counter(uint8_t value)
{
    if (pit.access == 1) {
        pit.reload = value;
    } else if (pit.access == 2) {
        pit.reload = (uint16_t)(value << 8);
    } else if (pit.low_next) {
        pit.reload = (uint16_t)((pit.reload & 0xFF00) | value);
        pit.low_next = 0;
        return;
    } else {
        pit.reload = (uint16_t)((pit.reload & 0x00FF) | (value << 8));
        pit.low_next = 1;
    }
    uint32_t count = pit.reload ? pit.reload : 0x10000;
    pit.period_ns = (int64_t)count * 1000000000LL / PIT_CLOCK;
    pit.next_ns = now_ns + pit.period_ns;
    rt_next_check_icount = 0;
}

static uint8_t pit_read(void)
{
    if (pit.read_low_next) pit.latched = pit_count_left();
    uint8_t value = pit.read_low_next ? (uint8_t)pit.latched : (uint8_t)(pit.latched >> 8);
    pit.read_low_next = !pit.read_low_next;
    return value;
}

static uint8_t port_read8(Cpu *c, uint16_t port)
{
    uint8_t video_value;
    if (video_in8(port, &video_value)) return video_value;
    switch (port) {
    case 0x00:
        return (uint8_t)irq_in_service;
    case 0x02:
        return master_mask;
    case 0x08:
        return (uint8_t)(irq_in_service >> 8);
    case 0x0A:
        return slave_mask;
    case 0x31:
        return dip_switch_31;
    case 0x42:
        return 0x94;
    case 0x71:
        update_time(c);
        return pit_read();
    case 0x60:
    case 0xA0:
        return read_gdc_status(c);
    case 0xA460:
        return board == BOARD_86 ? (uint8_t)(0x40 | board86_mask) : 0xFF;
    case 0x188:
    case 0x18A:
    case 0x18C:
    case 0x18E:
        update_time(c);
        return opn_read((port & 4) ? 1 : 0, (port & 2) != 0);
    default:
        if ((port & 0xF0F1) == 0x0080) return 0xFF;
        rt_log("in8 port %04x -> ff (not modelled)", port);
        return 0xFF;
    }
}


static void port_write8(Cpu *c, uint16_t port, uint8_t value)
{
    if (video_out8(port, value)) return;
    switch (port) {
    case 0x00:
        end_of_interrupt(0, value);
        update_interrupt_waiting();
        return;
    case 0x02:
        master_mask = value;
        update_interrupt_waiting();
        return;
    case 0x08:
        end_of_interrupt(1, value);
        update_interrupt_waiting();
        return;
    case 0x0A:
        slave_mask = value;
        update_interrupt_waiting();
        return;
    case 0x5F:
        return;
    case 0x64:
        vsync_armed = 1;
        return;
    case 0x71:
        update_time(c);
        pit_counter(value);
        return;
    case 0x73:
    case 0x75:
        return;
    case 0x77:
        pit_control(value);
        return;
    case 0xA460:
        board86_mask = value & 0x03;
        return;
    case 0x188:
    case 0x18A:
    case 0x18C:
    case 0x18E:
        update_time(c);
        opn_write((port & 4) ? 1 : 0, (port & 2) != 0, value);
        return;
    default:
        if ((port & 0xFCF9) == 0x0088) return;
        rt_log("out8 port %04x = %02x (not modelled)", port, value);
    }
}

/* Port access time on the C-bus (9.8304 MHz): 8.5 bus clocks for 8-bit, 5.5 for 16-bit, writes 3/4 of that, as
 * DOSBox-X charges it (iodelay with "cbus bus clock" std10). */
#define PORT_READ8_NS 865
#define PORT_WRITE8_NS 649
#define PORT_READ16_NS 560
#define PORT_WRITE16_NS 420

static double port_time_remainder;

static void charge_port_ns(Cpu *c, double nanoseconds)
{
    port_time_remainder += nanoseconds * (double)cycles / 1000000.0;
    uint64_t whole = (uint64_t)port_time_remainder;
    c->icount += whole;
    port_time_remainder -= (double)whole;
}

uint8_t rt_in8(Cpu *c, uint16_t port)
{
    rt_changes++;
    charge_port_ns(c, PORT_READ8_NS);
    return port_read8(c, port);
}

uint16_t rt_in16(Cpu *c, uint16_t port)
{
    rt_changes++;
    charge_port_ns(c, PORT_READ16_NS);
    return (uint16_t)(port_read8(c, port) | (port_read8(c, (uint16_t)(port + 1)) << 8));
}

void rt_out8(Cpu *c, uint16_t port, uint8_t value)
{
    rt_changes++;
    charge_port_ns(c, PORT_WRITE8_NS);
    port_write8(c, port, value);
}

void rt_out16(Cpu *c, uint16_t port, uint16_t value)
{
    rt_changes++;
    charge_port_ns(c, PORT_WRITE16_NS);
    port_write8(c, port, (uint8_t)value);
    port_write8(c, (uint16_t)(port + 1), (uint8_t)(value >> 8));
}

/* Snapshot: interrupt controller, PIT, sound board ports, and the emulated time (with c->icount the frame counter
 * continues as in the session that saved). */
void pc98_snapshot(StateStream *s)
{
    STATE(s, board86_mask);
    STATE(s, opn_address);
    STATE(s, dip_switch_31);
    STATE(s, master_mask);
    STATE(s, slave_mask);
    STATE(s, irq_pending);
    STATE(s, irq_in_service);
    STATE(s, sound_edges);
    STATE(s, vsync_armed);
    STATE(s, frames_delivered);
    STATE(s, last_frame);
    STATE(s, idle_ns);
    STATE(s, now_ns);
    STATE(s, pit);
    STATE(s, pit_due_ns);
    STATE(s, port_time_remainder);
    STATE(s, gdc_last_read_icount);
    STATE(s, gdc_last_value);
}

/* After a load the emulated time jumps: hold it to the wall clock from here, and check events at once. */
void pc98_after_load(void)
{
    wall_start_ns = wall_ns() - now_ns;
    rt_next_check_icount = 0;
    input_after_load(current_frame());
}
