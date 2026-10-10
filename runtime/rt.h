/* Runtime for recompiled real-mode x86 code (prototype). */
#ifndef RUSTY_RT_H
#define RUSTY_RT_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

typedef union {
    uint16_t x;
    struct {
        uint8_t l;
        uint8_t h;
    } b;
} Reg16;

struct RtModule;

typedef struct Cpu {
    Reg16 ax, bx, cx, dx;
    uint16_t si, di, bp, sp;
    uint16_t cs, ds, es, ss;
    uint16_t flags;
    uint16_t ret_ip, ret_cs;
    uint8_t *mem;
    const struct RtModule *module;
    uint16_t load_seg;
    uint64_t icount;
} Cpu;

typedef void (*RtFunc)(Cpu *c);

/* The binary keeps no original bytes, only FNV-1a 64 hashes of them. "print_hash" = hash of the first code bytes
 * of the function in the image (only its own instructions, print_length bytes, at most 8): identifies the module
 * when its signature bytes (data at the image start) have changed. */
typedef struct {
    uint16_t segment;
    uint16_t offset;
    RtFunc func;
    uint64_t print_hash;
    uint8_t print_length;
} RtEntry;

/* A recompiled module: its image starts at offset "base" of its segment. "signature_hash" = hash of the first 32
 * image bytes, where a byte with a clear bit in "mask" counts as 0 (the module writes it itself: CS variables such
 * as overlay entry slots). */
#define RT_SIGNATURE_SIZE 32

typedef struct RtModule {
    const char *name;
    uint16_t base;
    uint64_t signature_hash;
    uint32_t mask;
    const RtEntry *entries;
} RtModule;

/* The link table: all recompiled modules, the program named on the command line, and the resident drivers that
 * the loader runs first ("FILE.COM options"). */
extern const RtModule *const rt_modules[];
extern const RtModule *const rt_main_module;
extern const char *const rt_drivers[];

/* The original file of a module and the FNV-1a 64 hash of its bytes: at start the runtime hashes the game files and
 * uses only the modules whose file matches (the Japanese and English code files share their first bytes). */
typedef struct {
    const RtModule *module;
    const char *file;
    uint64_t hash;
} RtModuleFile;

extern const RtModuleFile rt_module_files[];

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

#define FLAG_CF(c) (((c)->flags & F_CF) != 0)
#define FLAG_PF(c) (((c)->flags & F_PF) != 0)
#define FLAG_ZF(c) (((c)->flags & F_ZF) != 0)
#define FLAG_SF(c) (((c)->flags & F_SF) != 0)
#define FLAG_OF(c) (((c)->flags & F_OF) != 0)
#define STEP(n) ((c->flags & F_DF) ? -(n) : (n))

#define RT_MEMORY_SIZE 0x110000

static inline uint32_t rt_linear(uint16_t seg, uint16_t off)
{
    return (((uint32_t)seg << 4) + off) & 0xFFFFF;
}

uint8_t rt_mmio_read8(Cpu *c, uint32_t linear);
void rt_mmio_write8(Cpu *c, uint32_t linear, uint8_t value);

static inline uint8_t rd8(Cpu *c, uint16_t seg, uint16_t off)
{
    uint32_t linear = rt_linear(seg, off);
    if (linear >= 0xA8000 && linear < 0xE8000) return rt_mmio_read8(c, linear);
    return c->mem[linear];
}

uint16_t rt_mmio_read16(Cpu *c, uint32_t linear);
void rt_mmio_write16(Cpu *c, uint32_t linear, uint16_t value);

static inline uint16_t rd16(Cpu *c, uint16_t seg, uint16_t off)
{
    uint32_t linear = rt_linear(seg, off);
    if (linear >= 0xA8000 && linear < 0xE8000 && !(off & 1)) return rt_mmio_read16(c, linear);
    return (uint16_t)(rd8(c, seg, off) | (rd8(c, seg, (uint16_t)(off + 1)) << 8));
}

/* Counts every event that can change the machine state or depend on the time: a memory write that changes a byte,
 * VRAM/EGC accesses, port I/O, interrupt calls, event checks. rt_poll_loop uses it to find idle loops. */
extern uint64_t rt_changes;

static inline void wr8(Cpu *c, uint16_t seg, uint16_t off, uint8_t value)
{
    uint32_t linear = rt_linear(seg, off);
    if (linear >= 0xA8000 && linear < 0xE8000) {
        rt_mmio_write8(c, linear, value);
        return;
    }
    if (c->mem[linear] != value) rt_changes++;
    c->mem[linear] = value;
}

static inline void wr16(Cpu *c, uint16_t seg, uint16_t off, uint16_t value)
{
    uint32_t linear = rt_linear(seg, off);
    if (linear >= 0xA8000 && linear < 0xE8000 && !(off & 1)) {
        rt_mmio_write16(c, linear, value);
        return;
    }
    wr8(c, seg, off, (uint8_t)value);
    wr8(c, seg, (uint16_t)(off + 1), (uint8_t)(value >> 8));
}

static inline void push16(Cpu *c, uint16_t value)
{
    c->sp -= 2;
    wr16(c, c->ss, c->sp, value);
}

static inline uint16_t pop16(Cpu *c)
{
    uint16_t value = rd16(c, c->ss, c->sp);
    c->sp += 2;
    return value;
}

/* Flags of the ALU operations, inline: the generated code calls them often. */
static inline uint16_t alu_parity(uint8_t value)
{
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return (value & 1) ? 0 : F_PF;
}

static inline void alu_szp8(Cpu *c, uint8_t r)
{
    c->flags |= alu_parity(r) | (r == 0 ? F_ZF : 0) | ((r & 0x80) ? F_SF : 0);
}

static inline void alu_szp16(Cpu *c, uint16_t r)
{
    c->flags |= alu_parity((uint8_t)r) | (r == 0 ? F_ZF : 0) | ((r & 0x8000) ? F_SF : 0);
}

static inline uint8_t alu_add8c(Cpu *c, uint8_t a, uint8_t b, unsigned carry)
{
    unsigned r = (unsigned)a + b + carry;
    uint8_t v = (uint8_t)r;
    c->flags &= ~F_ARITH;
    c->flags |= (r > 0xFF ? F_CF : 0) | (((a ^ b ^ v) & 0x10) ? F_AF : 0) | (((a ^ v) & (b ^ v) & 0x80) ? F_OF : 0);
    alu_szp8(c, v);
    return v;
}

static inline uint16_t alu_add16c(Cpu *c, uint16_t a, uint16_t b, unsigned carry)
{
    uint32_t r = (uint32_t)a + b + carry;
    uint16_t v = (uint16_t)r;
    c->flags &= ~F_ARITH;
    c->flags |= (r > 0xFFFF ? F_CF : 0) | (((a ^ b ^ v) & 0x10) ? F_AF : 0) | (((a ^ v) & (b ^ v) & 0x8000) ? F_OF : 0);
    alu_szp16(c, v);
    return v;
}

static inline uint8_t alu_sub8c(Cpu *c, uint8_t a, uint8_t b, unsigned borrow)
{
    unsigned r = (unsigned)a - b - borrow;
    uint8_t v = (uint8_t)r;
    c->flags &= ~F_ARITH;
    c->flags |= (((unsigned)b + borrow > a) ? F_CF : 0) | (((a ^ b ^ v) & 0x10) ? F_AF : 0) | (((a ^ b) & (a ^ v) & 0x80) ? F_OF : 0);
    alu_szp8(c, v);
    return v;
}

static inline uint16_t alu_sub16c(Cpu *c, uint16_t a, uint16_t b, unsigned borrow)
{
    uint32_t r = (uint32_t)a - b - borrow;
    uint16_t v = (uint16_t)r;
    c->flags &= ~F_ARITH;
    c->flags |= (((uint32_t)b + borrow > a) ? F_CF : 0) | (((a ^ b ^ v) & 0x10) ? F_AF : 0) | (((a ^ b) & (a ^ v) & 0x8000) ? F_OF : 0);
    alu_szp16(c, v);
    return v;
}

static inline uint8_t alu_add8(Cpu *c, uint8_t a, uint8_t b) { return alu_add8c(c, a, b, 0); }
static inline uint16_t alu_add16(Cpu *c, uint16_t a, uint16_t b) { return alu_add16c(c, a, b, 0); }
static inline uint8_t alu_adc8(Cpu *c, uint8_t a, uint8_t b) { return alu_add8c(c, a, b, FLAG_CF(c)); }
static inline uint16_t alu_adc16(Cpu *c, uint16_t a, uint16_t b) { return alu_add16c(c, a, b, FLAG_CF(c)); }
static inline uint8_t alu_sub8(Cpu *c, uint8_t a, uint8_t b) { return alu_sub8c(c, a, b, 0); }
static inline uint16_t alu_sub16(Cpu *c, uint16_t a, uint16_t b) { return alu_sub16c(c, a, b, 0); }
static inline uint8_t alu_sbb8(Cpu *c, uint8_t a, uint8_t b) { return alu_sub8c(c, a, b, FLAG_CF(c)); }
static inline uint16_t alu_sbb16(Cpu *c, uint16_t a, uint16_t b) { return alu_sub16c(c, a, b, FLAG_CF(c)); }

static inline uint8_t alu_logic8(Cpu *c, uint8_t v)
{
    c->flags &= ~F_ARITH;
    alu_szp8(c, v);
    return v;
}

static inline uint16_t alu_logic16(Cpu *c, uint16_t v)
{
    c->flags &= ~F_ARITH;
    alu_szp16(c, v);
    return v;
}

static inline uint8_t alu_and8(Cpu *c, uint8_t a, uint8_t b) { return alu_logic8(c, a & b); }
static inline uint16_t alu_and16(Cpu *c, uint16_t a, uint16_t b) { return alu_logic16(c, a & b); }
static inline uint8_t alu_or8(Cpu *c, uint8_t a, uint8_t b) { return alu_logic8(c, a | b); }
static inline uint16_t alu_or16(Cpu *c, uint16_t a, uint16_t b) { return alu_logic16(c, a | b); }
static inline uint8_t alu_xor8(Cpu *c, uint8_t a, uint8_t b) { return alu_logic8(c, a ^ b); }
static inline uint16_t alu_xor16(Cpu *c, uint16_t a, uint16_t b) { return alu_logic16(c, a ^ b); }

static inline uint8_t alu_inc8(Cpu *c, uint8_t a)
{
    uint16_t carry = c->flags & F_CF;
    uint8_t v = alu_add8c(c, a, 1, 0);
    c->flags = (c->flags & ~F_CF) | carry;
    return v;
}

static inline uint16_t alu_inc16(Cpu *c, uint16_t a)
{
    uint16_t carry = c->flags & F_CF;
    uint16_t v = alu_add16c(c, a, 1, 0);
    c->flags = (c->flags & ~F_CF) | carry;
    return v;
}

static inline uint8_t alu_dec8(Cpu *c, uint8_t a)
{
    uint16_t carry = c->flags & F_CF;
    uint8_t v = alu_sub8c(c, a, 1, 0);
    c->flags = (c->flags & ~F_CF) | carry;
    return v;
}

static inline uint16_t alu_dec16(Cpu *c, uint16_t a)
{
    uint16_t carry = c->flags & F_CF;
    uint16_t v = alu_sub16c(c, a, 1, 0);
    c->flags = (c->flags & ~F_CF) | carry;
    return v;
}

static inline uint8_t alu_neg8(Cpu *c, uint8_t a) { return alu_sub8c(c, 0, a, 0); }
static inline uint16_t alu_neg16(Cpu *c, uint16_t a) { return alu_sub16c(c, 0, a, 0); }

uint8_t alu_shl8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_shl16(Cpu *c, uint16_t a, uint8_t count);
uint8_t alu_shr8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_shr16(Cpu *c, uint16_t a, uint8_t count);
uint8_t alu_sar8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_sar16(Cpu *c, uint16_t a, uint8_t count);
uint8_t alu_rol8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_rol16(Cpu *c, uint16_t a, uint8_t count);
uint8_t alu_ror8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_ror16(Cpu *c, uint16_t a, uint8_t count);
uint8_t alu_rcl8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_rcl16(Cpu *c, uint16_t a, uint8_t count);
uint8_t alu_rcr8(Cpu *c, uint8_t a, uint8_t count);
uint16_t alu_rcr16(Cpu *c, uint16_t a, uint8_t count);

void op_mul8(Cpu *c, uint8_t v);
void op_mul16(Cpu *c, uint16_t v);
void op_imul8(Cpu *c, uint8_t v);
void op_imul16(Cpu *c, uint16_t v);
uint16_t op_imul_trunc16(Cpu *c, uint16_t a, uint16_t b);
void op_div8(Cpu *c, uint8_t v);
void op_div16(Cpu *c, uint16_t v);
void op_idiv8(Cpu *c, uint8_t v);
void op_idiv16(Cpu *c, uint16_t v);
void op_pusha(Cpu *c);
void op_popa(Cpu *c);
void op_pushf(Cpu *c);
void op_popf(Cpu *c);
void op_cbw(Cpu *c);
void op_cwd(Cpu *c);
void op_lahf(Cpu *c);
void op_sahf(Cpu *c);
void op_xlatb(Cpu *c);
void op_enter(Cpu *c, uint16_t size, uint8_t level);

void rt_int(Cpu *c, uint8_t number);
void pc98_init(Cpu *c);
void dos_init(Cpu *c, uint16_t first_free);
int dos_run(Cpu *c, const char *name, const char *tail);
int dos_run_file(Cpu *c, FILE *file, const char *name, const char *tail);
int exepack_detect(const uint8_t *data, size_t size);
size_t exepack_unpack(const uint8_t *data, size_t size, uint8_t *out, size_t capacity);
void bios_init(Cpu *c);
void rt_install_bios_stubs(Cpu *c);
void video_init(const uint8_t *cpu_memory);
void video_set_graphics_mode(uint8_t ch);
void video_set_graphics_on(int on);
void video_set_egc_available(int available);
void video_set_text_on(int on);
int video_out8(uint16_t port, uint8_t value);
int video_in8(uint16_t port, uint8_t *value);
const uint8_t *video_render(void);

/* Raw display state for the GPU decode (gpu.c): the same inputs as video_render(). */
typedef struct {
    const uint8_t *planes; /* displayed page: planes B, R, G, I of 8000h bytes each */
    const uint8_t *text; /* text VRAM A0000h-A3FFFh: codes, attributes at +2000h */
    const uint8_t *font;
    uint32_t font_size;
    uint32_t font_version; /* changes when the program writes user-defined glyphs (CG RAM) */
    uint8_t palette[16][3]; /* 4-bit values */
    uint32_t area_start[2];
    int area0_lines;
    int lines_400;
    int graphics_on;
    int text_on;
} VideoState;

void video_state(VideoState *state);
void video_frame(long frame);
int display_init(void);
void display_message(const char *text, int seconds);
void display_fatal(const char *text);
void display_frame(void);
int display_key_down(uint8_t code);
int display_key_queued(uint8_t *code, int remove);
void display_key_push(uint8_t code);
void display_key_set(uint8_t code, int down);
void display_key_hold(uint8_t code, int down);
double display_audio_queued_seconds(void);
void input_init(void);
int input_replaying(long frame);
void input_frame(long previous, long frame);
void input_host_key(long frame, uint8_t code, int down);
void input_host_special(long frame, uint8_t code);
void input_after_load(long frame);
long long input_base_time(void);
int input_stage(void);
int disk_is_image(const char *path);
int disk_find_game(const char *dir, char *out, size_t size);
int disk_open_game(const char *path);
void disk_use_folder(const char *dir);
const char *disk_source(void);
void disk_host_folder(char *out, size_t size);
int disk_host_file(const char *dir, const char *name, char *out, size_t size);
/* An original game file: a host file (game folder) or the data of a disk image in memory. */
typedef struct {
    FILE *file;
    const uint8_t *data;
    long size;
} DiskFile;
int disk_open_file(const char *name, DiskFile *out);
const char *dos_user_dir(void);
const char *dos_save_dir(void);
long dos_read_game_file(const char *name, uint8_t *buffer, size_t capacity);
long dos_read_original_file(const char *name, uint8_t *buffer, size_t capacity);
void dos_write_save_file(const char *name, const uint8_t *data, long size);
void input_save(long frame);
const char *input_save_path(void);
void pc98_host_key(uint8_t code, int down);
void pc98_toggle_godmode(void);
void cheats_set_godmode(int on);
void cheats_frame(Cpu *c);
long long pc98_clock_seconds(void);
double pc98_time_ms(void);
void pc98_charge_us(Cpu *c, double microseconds);
void rt_poll_events(Cpu *c);
/* Called at every backward jump: the full event check runs only when the instruction count reaches the next
 * planned event or an unmasked interrupt waits. */
extern uint64_t rt_next_check_icount;
extern int rt_interrupt_waiting;
static inline void rt_poll(Cpu *c)
{
    if (c->icount >= rt_next_check_icount || rt_interrupt_waiting) rt_poll_events(c);
}

/* Before each backward jump. A pass of a loop that ends at the same jump with the same registers and flags as the
 * pass before, and changed nothing in between (rt_changes), repeats exactly until the next event: rt_loop_repeat
 * then adds the instruction count of the passes up to that event at once, as if they ran. */
extern uint32_t rt_loop_site;
extern uint64_t rt_loop_changes;
extern int rt_loop_saved;
void rt_loop_repeat(Cpu *c);

static inline void rt_poll_loop(Cpu *c, uint16_t address)
{
    uint32_t site = ((uint32_t)c->cs << 16) | address;
    if (site == rt_loop_site && rt_changes == rt_loop_changes) {
        rt_loop_repeat(c);
    } else {
        rt_loop_site = site;
        rt_loop_changes = rt_changes;
        rt_loop_saved = 0;
    }
    rt_poll(c);
}

/* After STI / POPF: a waiting interrupt is taken at once, as the CPU does after the next instruction. */
static inline void rt_poll_interrupts(Cpu *c)
{
    if (rt_interrupt_waiting && (c->flags & F_IF)) rt_poll_events(c);
}
int rt_bios_int(Cpu *c, uint8_t number);
uint16_t dos_reserve(uint16_t paragraphs);
uint8_t rt_in8(Cpu *c, uint16_t port);
uint16_t rt_in16(Cpu *c, uint16_t port);
void rt_out8(Cpu *c, uint16_t port, uint8_t value);
void rt_out16(Cpu *c, uint16_t port, uint16_t value);
void rt_halt(Cpu *c);
void rt_call_near(Cpu *c, uint16_t target);
void rt_jump_near(Cpu *c, uint16_t target);
int rt_check_return(Cpu *c, uint16_t expected);
void rt_call_far(Cpu *c, uint16_t segment, uint16_t offset);
void rt_jump_far(Cpu *c, uint16_t segment, uint16_t offset);
void rt_check_far_return(Cpu *c, uint16_t segment, uint16_t offset);
void rt_register_instance(const RtModule *module, uint16_t load_seg, uint16_t paragraphs);
void rt_unregister_instance(uint16_t load_seg);
void rt_resize_instance(uint16_t load_seg, uint16_t paragraphs);
void rt_select_modules(void);
void rt_note_open(const char *name);
const char *rt_game_version_tag(void);
void rt_program_folder(char *out, size_t size);
void platform_make_folder(const char *path);
int platform_exists(const char *path);
int platform_writable(const char *path);
int platform_replace_file(const char *from, const char *to);
void platform_user_data_folder(char *out, size_t size);
void platform_setup_log(void);
const RtModule *rt_find_module(Cpu *c, uint16_t load_seg);
int rt_instance_segment(const char *name, uint16_t *load_seg);
int rt_vector_is_recompiled(Cpu *c, uint8_t number);
void rt_call_interrupt(Cpu *c, uint8_t number);
void rt_start(Cpu *c, uint16_t segment, uint16_t offset);
void rt_fell_off(Cpu *c, uint16_t address);
void rt_interpret(Cpu *c, uint16_t ip);
RtFunc rt_find_function(const RtModule *module, uint16_t image_segment, uint16_t offset);
void rt_fatal(const char *format, ...);
void rt_log(const char *format, ...);
long pc98_frame(void);
void pc98_end_of_interrupt(int slave);
void pc98_idle(Cpu *c);
void console_write(Cpu *c, uint8_t ch);

/* Snapshot save states (state.c): saved and loaded only at a safe point of the game (rt_safe_point, the top of
 * JO.EXE's main loop), where the C call chain is always the same. Each part of the runtime writes or reads its state
 * with state_io, in the same order for both directions. */
typedef struct StateStream StateStream;
void state_io(StateStream *s, void *data, size_t size);
int state_loading(const StateStream *s);
#define STATE(s, variable) state_io((s), &(variable), sizeof(variable))
void rt_safe_point(Cpu *c, uint16_t address);
void state_frame(void);
void state_request(int load);
void state_change_slot(int delta);
void rt_snapshot(StateStream *s);
uint64_t rt_context_hash(const Cpu *c);
int rt_current_story(void);
void pc98_snapshot(StateStream *s);
void pc98_after_load(void);
void video_snapshot(StateStream *s);
void dos_snapshot(StateStream *s);
int dos_open_handles(void);
uint32_t dos_context(void);
void console_snapshot(StateStream *s);
void display_snapshot(StateStream *s);
void display_after_load(void);
void input_snapshot(StateStream *s);
void cheats_snapshot(StateStream *s);

#endif
