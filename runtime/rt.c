/* CPU helpers: flags for ALU operations, multiply/divide, stack and control flow checks. */
#include "rt.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint64_t rt_changes;
uint32_t rt_loop_site;
uint64_t rt_loop_changes;
int rt_loop_saved;
static Cpu loop_registers;
static uint64_t loop_icount;

/* The registers and flags (the fields before ret_cs, and ret_cs): the state a pass of a loop works on. */
#define LOOP_STATE_SIZE (offsetof(Cpu, ret_cs) + sizeof(uint16_t))

/* RUSTY_LOOP_SKIP=0 runs every pass (for the test that compares both ways). */
static int loop_skip = -1;

void rt_loop_repeat(Cpu *c)
{
    if (loop_skip < 0) {
        const char *setting = getenv("RUSTY_LOOP_SKIP");
        loop_skip = !(setting && strcmp(setting, "0") == 0);
    }
    if (!loop_skip) return;
    if (!rt_loop_saved || memcmp(c, &loop_registers, LOOP_STATE_SIZE) != 0) {
        memcpy(&loop_registers, c, LOOP_STATE_SIZE);
        loop_icount = c->icount;
        rt_loop_saved = 1;
        return;
    }
    uint64_t pass = c->icount - loop_icount;
    if (pass > 0 && !rt_interrupt_waiting && c->icount < rt_next_check_icount)
        c->icount += (rt_next_check_icount - c->icount + pass - 1) / pass * pass;
    loop_icount = c->icount;
}

/* The 80186 and later mask the shift count to 5 bits; a count of 0 changes nothing. */
#define SHIFT_PREPARE() \
    count &= 0x1F;      \
    if (count == 0) return a;

uint8_t alu_shl8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned r = (unsigned)a << count;
    uint8_t v = (uint8_t)r;
    c->flags &= ~F_ARITH;
    c->flags |= ((r & 0x100) ? F_CF : 0);
    alu_szp8(c, v);
    if (((v & 0x80) != 0) != FLAG_CF(c)) c->flags |= F_OF;
    return v;
}

uint16_t alu_shl16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    uint32_t r = (uint32_t)a << count;
    uint16_t v = (uint16_t)r;
    c->flags &= ~F_ARITH;
    c->flags |= ((r & 0x10000) ? F_CF : 0);
    alu_szp16(c, v);
    if (((v & 0x8000) != 0) != FLAG_CF(c)) c->flags |= F_OF;
    return v;
}

uint8_t alu_shr8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    uint8_t v = (uint8_t)(count > 8 ? 0 : a >> count);
    c->flags &= ~F_ARITH;
    c->flags |= ((count <= 8 && ((a >> (count - 1)) & 1)) ? F_CF : 0) | ((a & 0x80) ? F_OF : 0);
    alu_szp8(c, v);
    return v;
}

uint16_t alu_shr16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    uint16_t v = (uint16_t)(count > 16 ? 0 : a >> count);
    c->flags &= ~F_ARITH;
    c->flags |= ((count <= 16 && ((a >> (count - 1)) & 1)) ? F_CF : 0) | ((a & 0x8000) ? F_OF : 0);
    alu_szp16(c, v);
    return v;
}

uint8_t alu_sar8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    int8_t s = (int8_t)a;
    unsigned n = count > 7 ? 7 : count;
    uint8_t v = (uint8_t)(s >> n);
    c->flags &= ~F_ARITH;
    c->flags |= (((s >> (count > 8 ? 7 : count - 1)) & 1) ? F_CF : 0);
    alu_szp8(c, v);
    return v;
}

uint16_t alu_sar16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    int16_t s = (int16_t)a;
    unsigned n = count > 15 ? 15 : count;
    uint16_t v = (uint16_t)(s >> n);
    c->flags &= ~F_ARITH;
    c->flags |= (((s >> (count > 16 ? 15 : count - 1)) & 1) ? F_CF : 0);
    alu_szp16(c, v);
    return v;
}

uint8_t alu_rol8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned n = count & 7;
    uint8_t v = (uint8_t)((a << n) | (a >> ((8 - n) & 7)));
    c->flags = (c->flags & ~(F_CF | F_OF)) | ((v & 1) ? F_CF : 0);
    if (((v & 0x80) != 0) != FLAG_CF(c)) c->flags |= F_OF;
    return v;
}

uint16_t alu_rol16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned n = count & 15;
    uint16_t v = (uint16_t)((a << n) | (a >> ((16 - n) & 15)));
    c->flags = (c->flags & ~(F_CF | F_OF)) | ((v & 1) ? F_CF : 0);
    if (((v & 0x8000) != 0) != FLAG_CF(c)) c->flags |= F_OF;
    return v;
}

uint8_t alu_ror8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned n = count & 7;
    uint8_t v = (uint8_t)((a >> n) | (a << ((8 - n) & 7)));
    c->flags = (c->flags & ~(F_CF | F_OF)) | ((v & 0x80) ? F_CF : 0);
    if (((v ^ (v << 1)) & 0x80) != 0) c->flags |= F_OF;
    return v;
}

uint16_t alu_ror16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned n = count & 15;
    uint16_t v = (uint16_t)((a >> n) | (a << ((16 - n) & 15)));
    c->flags = (c->flags & ~(F_CF | F_OF)) | ((v & 0x8000) ? F_CF : 0);
    if (((v ^ (v << 1)) & 0x8000) != 0) c->flags |= F_OF;
    return v;
}

uint8_t alu_rcl8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned carry = FLAG_CF(c);
    for (unsigned i = 0; i < count % 9; i++) {
        unsigned out = a >> 7;
        a = (uint8_t)((a << 1) | carry);
        carry = out;
    }
    c->flags = (c->flags & ~(F_CF | F_OF)) | (carry ? F_CF : 0);
    if (((a & 0x80) != 0) != (carry != 0)) c->flags |= F_OF;
    return a;
}

uint16_t alu_rcl16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned carry = FLAG_CF(c);
    for (unsigned i = 0; i < count % 17; i++) {
        unsigned out = a >> 15;
        a = (uint16_t)((a << 1) | carry);
        carry = out;
    }
    c->flags = (c->flags & ~(F_CF | F_OF)) | (carry ? F_CF : 0);
    if (((a & 0x8000) != 0) != (carry != 0)) c->flags |= F_OF;
    return a;
}

uint8_t alu_rcr8(Cpu *c, uint8_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned carry = FLAG_CF(c);
    uint16_t of = (uint16_t)((((a >> 7) & 1) != carry) ? F_OF : 0);
    for (unsigned i = 0; i < count % 9; i++) {
        unsigned out = a & 1;
        a = (uint8_t)((a >> 1) | (carry << 7));
        carry = out;
    }
    c->flags = (c->flags & ~(F_CF | F_OF)) | (carry ? F_CF : 0) | of;
    return a;
}

uint16_t alu_rcr16(Cpu *c, uint16_t a, uint8_t count)
{
    SHIFT_PREPARE()
    unsigned carry = FLAG_CF(c);
    uint16_t of = (uint16_t)((((a >> 15) & 1) != carry) ? F_OF : 0);
    for (unsigned i = 0; i < count % 17; i++) {
        unsigned out = a & 1;
        a = (uint16_t)((a >> 1) | (carry << 15));
        carry = out;
    }
    c->flags = (c->flags & ~(F_CF | F_OF)) | (carry ? F_CF : 0) | of;
    return a;
}

void op_mul8(Cpu *c, uint8_t v)
{
    c->ax.x = (uint16_t)(c->ax.b.l * v);
    c->flags = (c->flags & ~(F_CF | F_OF)) | (c->ax.b.h ? (F_CF | F_OF) : 0);
}

void op_mul16(Cpu *c, uint16_t v)
{
    uint32_t r = (uint32_t)c->ax.x * v;
    c->ax.x = (uint16_t)r;
    c->dx.x = (uint16_t)(r >> 16);
    c->flags = (c->flags & ~(F_CF | F_OF)) | (c->dx.x ? (F_CF | F_OF) : 0);
}

void op_imul8(Cpu *c, uint8_t v)
{
    int16_t r = (int16_t)((int8_t)c->ax.b.l * (int8_t)v);
    c->ax.x = (uint16_t)r;
    c->flags = (c->flags & ~(F_CF | F_OF)) | (r != (int8_t)r ? (F_CF | F_OF) : 0);
}

void op_imul16(Cpu *c, uint16_t v)
{
    int32_t r = (int32_t)(int16_t)c->ax.x * (int16_t)v;
    c->ax.x = (uint16_t)r;
    c->dx.x = (uint16_t)((uint32_t)r >> 16);
    c->flags = (c->flags & ~(F_CF | F_OF)) | (r != (int16_t)r ? (F_CF | F_OF) : 0);
}

uint16_t op_imul_trunc16(Cpu *c, uint16_t a, uint16_t b)
{
    int32_t r = (int32_t)(int16_t)a * (int16_t)b;
    c->flags = (c->flags & ~(F_CF | F_OF)) | (r != (int16_t)r ? (F_CF | F_OF) : 0);
    return (uint16_t)r;
}

void op_div8(Cpu *c, uint8_t v)
{
    if (v == 0 || c->ax.x / v > 0xFF) rt_fatal("divide error (div8)");
    uint16_t a = c->ax.x;
    c->ax.b.l = (uint8_t)(a / v);
    c->ax.b.h = (uint8_t)(a % v);
}

void op_div16(Cpu *c, uint16_t v)
{
    uint32_t a = ((uint32_t)c->dx.x << 16) | c->ax.x;
    if (v == 0 || a / v > 0xFFFF) rt_fatal("divide error (div16)");
    c->ax.x = (uint16_t)(a / v);
    c->dx.x = (uint16_t)(a % v);
}

void op_idiv8(Cpu *c, uint8_t v)
{
    int16_t a = (int16_t)c->ax.x;
    if (v == 0) rt_fatal("divide error (idiv8)");
    int16_t q = (int16_t)(a / (int8_t)v);
    if (q != (int8_t)q) rt_fatal("divide error (idiv8 overflow)");
    c->ax.b.l = (uint8_t)q;
    c->ax.b.h = (uint8_t)(a % (int8_t)v);
}

void op_idiv16(Cpu *c, uint16_t v)
{
    int32_t a = (int32_t)(((uint32_t)c->dx.x << 16) | c->ax.x);
    if (v == 0) rt_fatal("divide error (idiv16)");
    int32_t q = a / (int16_t)v;
    if (q != (int16_t)q) rt_fatal("divide error (idiv16 overflow)");
    c->ax.x = (uint16_t)q;
    c->dx.x = (uint16_t)(a % (int16_t)v);
}

void op_pusha(Cpu *c)
{
    uint16_t sp = c->sp;
    push16(c, c->ax.x);
    push16(c, c->cx.x);
    push16(c, c->dx.x);
    push16(c, c->bx.x);
    push16(c, sp);
    push16(c, c->bp);
    push16(c, c->si);
    push16(c, c->di);
}

void op_popa(Cpu *c)
{
    c->di = pop16(c);
    c->si = pop16(c);
    c->bp = pop16(c);
    pop16(c);
    c->bx.x = pop16(c);
    c->dx.x = pop16(c);
    c->cx.x = pop16(c);
    c->ax.x = pop16(c);
}

void op_pushf(Cpu *c) { push16(c, (uint16_t)(c->flags | 0xF002)); }
void op_popf(Cpu *c) { c->flags = (uint16_t)((pop16(c) & 0x0FD5) | 0x0002); }
void op_cbw(Cpu *c) { c->ax.x = (uint16_t)(int16_t)(int8_t)c->ax.b.l; }
void op_cwd(Cpu *c) { c->dx.x = (c->ax.x & 0x8000) ? 0xFFFF : 0; }
void op_lahf(Cpu *c) { c->ax.b.h = (uint8_t)(c->flags | 0x02); }
void op_sahf(Cpu *c) { c->flags = (uint16_t)((c->flags & 0xFF00) | (c->ax.b.h & 0xD5) | 0x02); }
void op_xlatb(Cpu *c) { c->ax.b.l = rd8(c, c->ds, (uint16_t)(c->bx.x + c->ax.b.l)); }

void op_enter(Cpu *c, uint16_t size, uint8_t level)
{
    level &= 0x1F;
    push16(c, c->bp);
    uint16_t frame = c->sp;
    if (level > 0) {
        for (uint8_t i = 1; i < level; i++) {
            c->bp -= 2;
            push16(c, rd16(c, c->ss, c->bp));
        }
        push16(c, frame);
    }
    c->bp = frame;
    c->sp -= size;
}

static const RtEntry *find_entry_record(const RtModule *module, uint16_t segment, uint16_t offset)
{
    for (const RtEntry *entry = module->entries; entry->func; entry++) {
        if (entry->segment == segment && entry->offset == offset) return entry;
    }
    return 0;
}

static RtFunc find_entry(const RtModule *module, uint16_t segment, uint16_t offset)
{
    if (!module) return 0;
    const RtEntry *entry = find_entry_record(module, segment, offset);
    return entry ? entry->func : 0;
}

#define BIOS_SEGMENT 0xF000
#define MAX_INSTANCES 16
#define FAKE_RETURN_IP 0xFFFE

typedef struct {
    const RtModule *module;
    uint16_t load_seg;
    uint16_t paragraphs;
} Instance;

static Instance instances[MAX_INSTANCES];
static int instance_count;

void rt_register_instance(const RtModule *module, uint16_t load_seg, uint16_t paragraphs)
{
    if (instance_count < MAX_INSTANCES) instances[instance_count++] = (Instance){module, load_seg, paragraphs};
}

void rt_resize_instance(uint16_t load_seg, uint16_t paragraphs)
{
    for (int i = 0; i < instance_count; i++) {
        if (instances[i].load_seg == load_seg) instances[i].paragraphs = paragraphs;
    }
}

void rt_unregister_instance(uint16_t load_seg)
{
    for (int i = 0; i < instance_count; i++) {
        if (instances[i].load_seg == load_seg) {
            instances[i] = instances[--instance_count];
            return;
        }
    }
}

int rt_instance_segment(const char *name, uint16_t *load_seg)
{
    for (int i = instance_count - 1; i >= 0; i--) {
        if (strcmp(instances[i].module->name, name) == 0) {
            *load_seg = instances[i].load_seg;
            return 1;
        }
    }
    return 0;
}

#define MAX_MODULES 256
#define MAX_CODE_FILE_SIZE 0x100000

static const RtModule *active_modules[MAX_MODULES + 1];
static int modules_selected;
static const RtModule *opened_modules[MAX_MODULES];
static long opened_at[MAX_MODULES];
static long open_count;
static int english_version;

/* The modules to search: after rt_select_modules only those of the game version in the game folder. */
static const RtModule *const *module_list(void)
{
    return modules_selected ? active_modules : rt_modules;
}

static uint64_t file_hash(const uint8_t *data, long size)
{
    uint64_t value = 0xCBF29CE484222325ull;
    for (long i = 0; i < size; i++) value = (value ^ data[i]) * 0x100000001B3ull;
    return value;
}

/* The hash of the first RT_SIGNATURE_SIZE image bytes at linear address start; bytes with a clear mask bit count as
 * 0 (the module writes them itself). */
static uint64_t signature_hash(Cpu *c, uint32_t start, uint32_t mask)
{
    uint8_t window[RT_SIGNATURE_SIZE];
    for (int i = 0; i < RT_SIGNATURE_SIZE; i++) window[i] = mask >> i & 1 ? c->mem[(start + i) & 0xFFFFF] : 0;
    return file_hash(window, RT_SIGNATURE_SIZE);
}

/* The game opened a file: the modules of that code file become the most recent candidates (an overlay is read from
 * its file, then called). */
void rt_note_open(const char *name)
{
    for (const RtModuleFile *row = rt_module_files; row->module; row++) {
        if (strcasecmp(row->file, name) != 0) continue;
        for (int i = 0; i < MAX_MODULES; i++) {
            if (opened_modules[i] && opened_modules[i] != row->module) continue;
            opened_modules[i] = row->module;
            opened_at[i] = ++open_count;
            break;
        }
    }
}

#define STATE_NAME_SIZE 32

static const RtModule *module_by_name(const char *name)
{
    for (const RtModule *const *module = rt_modules; *module; module++) {
        if (strcmp((*module)->name, name) == 0) return *module;
    }
    return 0;
}

/* Snapshot: the order in which the game opened its code files (by module name), which chooses between modules that
 * match by code. */
void rt_snapshot(StateStream *s)
{
    int count = 0;
    while (count < MAX_MODULES && opened_modules[count]) count++;
    STATE(s, count);
    STATE(s, open_count);
    if (state_loading(s)) memset(opened_modules, 0, sizeof(opened_modules));
    for (int i = 0; i < count && i < MAX_MODULES; i++) {
        char name[STATE_NAME_SIZE] = {0};
        if (!state_loading(s)) snprintf(name, sizeof(name), "%s", opened_modules[i]->name);
        STATE(s, name);
        STATE(s, opened_at[i]);
        if (state_loading(s)) opened_modules[i] = module_by_name(name);
    }
}

/* The stage: the number n of the STORYn.COM overlay the game opened last (0 = none yet). */
int rt_current_story(void)
{
    int story = 0;
    long newest = 0;
    for (int i = 0; i < MAX_MODULES && opened_modules[i]; i++) {
        const char *name = strstr(opened_modules[i]->name, "STORY");
        if (name && opened_at[i] > newest) {
            newest = opened_at[i];
            story = atoi(name + 5);
        }
    }
    return story;
}

/* What must be the same for a snapshot load: the programs in memory (module, segment, size) and the running one. */
uint64_t rt_context_hash(const Cpu *c)
{
    uint64_t value = 0xCBF29CE484222325ull;
    for (int i = 0; i < instance_count; i++) {
        for (const char *p = instances[i].module->name; *p; p++) value = (value ^ (uint8_t)*p) * 0x100000001B3ull;
        value = (value ^ instances[i].load_seg) * 0x100000001B3ull;
        value = (value ^ instances[i].paragraphs) * 0x100000001B3ull;
    }
    for (const char *p = c->module ? c->module->name : ""; *p; p++) value = (value ^ (uint8_t)*p) * 0x100000001B3ull;
    return (value ^ c->load_seg) * 0x100000001B3ull;
}

static long last_opened(const RtModule *module)
{
    for (int i = 0; i < MAX_MODULES && opened_modules[i]; i++) {
        if (opened_modules[i] == module) return opened_at[i];
    }
    return 0;
}

static int has_file_row(const RtModule *module)
{
    for (const RtModuleFile *row = rt_module_files; row->module; row++) {
        if (row->module == module) return 1;
    }
    return 0;
}

void rt_select_modules(void)
{
    static uint8_t data[MAX_CODE_FILE_SIZE];
    static int matched[MAX_MODULES];
    int rows = 0;
    for (const RtModuleFile *row = rt_module_files; row->module && rows < MAX_MODULES; row++, rows++) {
        long size = dos_read_original_file(row->file, data, sizeof(data));
        matched[rows] = size < 0 ? -1 : file_hash(data, size) == row->hash;
    }
    if (!rows) return;
    for (int i = 0; i < rows; i++) {
        if (matched[i]) continue;
        int known = 0;
        for (int j = 0; j < rows; j++) {
            known |= matched[j] > 0 && strcmp(rt_module_files[j].file, rt_module_files[i].file) == 0;
        }
        if (known) continue;
        long size = dos_read_original_file(rt_module_files[i].file, data, sizeof(data));
        rt_fatal("%s/%s: unknown version of this code file (hash %016llx). Supported: the Japanese original and the "
                 "English translation by 46 OkuMen (v1.0.3)",
                 disk_source(), rt_module_files[i].file, (unsigned long long)file_hash(data, size));
    }
    int count = 0, english = 0;
    for (const RtModule *const *module = rt_modules; *module && count < MAX_MODULES; module++) {
        int use = !has_file_row(*module);
        for (int i = 0; i < rows; i++) use |= rt_module_files[i].module == *module && matched[i] > 0;
        if (!use) continue;
        active_modules[count++] = *module;
        english |= strncmp((*module)->name, "en/", 3) == 0;
    }
    active_modules[count] = 0;
    modules_selected = 1;
    english_version = english;
    rt_log("game version: %s (%d modules)", english ? "English (46 OkuMen v1.0.3)" : "Japanese", count);
}

#define HASH_CACHE_SIZE 8

const RtModule *rt_find_module(Cpu *c, uint16_t load_seg)
{
    struct {
        uint16_t base;
        uint32_t mask;
        uint64_t hash;
    } cache[HASH_CACHE_SIZE];
    int cached = 0;
    for (const RtModule *const *module = module_list(); *module; module++) {
        uint64_t hash = 0;
        int found = 0;
        for (int i = 0; i < cached && !found; i++) {
            found = cache[i].base == (*module)->base && cache[i].mask == (*module)->mask;
            hash = cache[i].hash;
        }
        if (!found) {
            hash = signature_hash(c, rt_linear(load_seg, (*module)->base), (*module)->mask);
            if (cached < HASH_CACHE_SIZE) {
                cache[cached].base = (*module)->base;
                cache[cached].mask = (*module)->mask;
                cache[cached++].hash = hash;
            }
        }
        if (hash == (*module)->signature_hash) return *module;
    }
    return 0;
}

static void bios_iret(Cpu *c)
{
    c->ret_ip = pop16(c);
    c->ret_cs = pop16(c);
    c->flags = pop16(c);
}

static void bios_irq_master(Cpu *c)
{
    pc98_end_of_interrupt(0);
    bios_iret(c);
}

static void bios_irq_slave(Cpu *c)
{
    pc98_end_of_interrupt(1);
    bios_iret(c);
}

static RtEntry bios_entries[257];
static const char bios_signature[] = "RUSTY BIOS";
static const RtModule bios_module = {"BIOS stub", 0x0000, 0, 0, bios_entries};

void rt_install_bios_stubs(Cpu *c)
{
    for (int n = 0; n < 256; n++) {
        bios_entries[n].segment = 0;
        bios_entries[n].offset = (uint16_t)(0x100 + n * 4);
        bios_entries[n].func = n >= 0x08 && n < 0x10 ? bios_irq_master : n >= 0x10 && n < 0x18 ? bios_irq_slave : bios_iret;
        if (rd16(c, 0, (uint16_t)(n * 4)) == 0 && rd16(c, 0, (uint16_t)(n * 4 + 2)) == 0) {
            wr16(c, 0, (uint16_t)(n * 4), (uint16_t)(0x100 + n * 4));
            wr16(c, 0, (uint16_t)(n * 4 + 2), BIOS_SEGMENT);
        }
    }
    for (int i = 0; bios_signature[i]; i++) wr8(c, BIOS_SEGMENT, (uint16_t)i, (uint8_t)bios_signature[i]);
}

static const RtModule *module_at(Cpu *c, uint16_t segment, uint16_t *load_seg)
{
    if (segment == BIOS_SEGMENT) {
        *load_seg = BIOS_SEGMENT;
        return &bios_module;
    }
    for (int i = instance_count - 1; i >= 0; i--) {
        if (segment >= instances[i].load_seg && segment < instances[i].load_seg + instances[i].paragraphs) {
            *load_seg = instances[i].load_seg;
            return instances[i].module;
        }
    }
    const RtModule *module = rt_find_module(c, segment);
    if (module) *load_seg = segment;
    return module;
}

static void record_missing(const RtModule *module, uint16_t image_segment, uint16_t offset);

RtFunc rt_find_function(const RtModule *module, uint16_t image_segment, uint16_t offset)
{
    return module ? find_entry(module, image_segment, offset) : 0;
}

static int interpreter_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RUSTY_NO_INTERPRETER") == 0;
    return enabled;
}

/* Code without a recompiled function: log it once (and into the missing log, so the next build recompiles it),
 * then interpret it. */
static void interpret_missing(Cpu *c, uint16_t offset, const char *what)
{
    uint16_t image_segment = (uint16_t)(c->cs - c->load_seg);
    const char *name = c->module ? c->module->name : "unknown code";
    if (!interpreter_enabled()) {
        if (c->module) record_missing(c->module, image_segment, offset);
        rt_fatal("%s: %s unknown address %04x:%04x", name, what, c->cs, offset);
    }
    static uint32_t seen[256];
    static int seen_count;
    uint32_t key = (uint32_t)image_segment << 16 | offset;
    int known = 0;
    for (int i = 0; i < seen_count; i++) known |= seen[i] == key;
    if (!known) {
        if (seen_count < 256) seen[seen_count++] = key;
        rt_log("%s: %s %04x:%04x is not recompiled: interpreted", name, what, image_segment, offset);
        if (c->module) record_missing(c->module, image_segment, offset);
    }
    rt_interpret(c, offset);
}

void rt_call_near(Cpu *c, uint16_t target)
{
    RtFunc func = find_entry(c->module, (uint16_t)(c->cs - c->load_seg), target);
    if (!func) {
        interpret_missing(c, target, "indirect call to");
        return;
    }
    func(c);
}

void rt_jump_near(Cpu *c, uint16_t target)
{
    RtFunc func = find_entry(c->module, (uint16_t)(c->cs - c->load_seg), target);
    if (!func) {
        interpret_missing(c, target, "indirect jump to");
        return;
    }
    func(c);
}

/* "en" or "ja": the game version found by rt_select_modules. */
const char *rt_game_version_tag(void)
{
    return english_version ? "en" : "ja";
}

/* Missing functions go to RUSTY_MISSING (default build/recomp/missing.txt; a release build writes none unless it is
 * set), so the next build recompiles them. */
static void record_missing(const RtModule *module, uint16_t image_segment, uint16_t offset)
{
    const char *path = getenv("RUSTY_MISSING");
#ifdef RUSTY_RELEASE
    if (!path) return;
#endif
    char default_path[600];
    snprintf(default_path, sizeof(default_path), "%s/missing.txt", dos_user_dir());
    FILE *file = fopen(path ? path : default_path, "a");
    if (!file) return;
    fprintf(file, "%s %04x:%04x\n", module->name, image_segment, offset);
    fclose(file);
}

/* The signature does not match (the module changed data in its first bytes): take the module whose function code
 * at segment:offset matches memory (by hash); with several candidates, the one whose file the game opened last. */
static const RtModule *module_by_code(Cpu *c, uint16_t segment, uint16_t offset, uint16_t *load_seg)
{
    const RtModule *best = 0;
    long best_opened = -1;
    uint32_t start = rt_linear(segment, offset);
    for (const RtModule *const *module = module_list(); *module; module++) {
        const RtEntry *entry = find_entry_record(*module, 0, offset);
        if (!entry || !entry->print_length || start + entry->print_length > RT_MEMORY_SIZE) continue;
        if (file_hash(&c->mem[start], entry->print_length) != entry->print_hash) continue;
        long opened = last_opened(*module);
        if (opened > best_opened) {
            best_opened = opened;
            best = *module;
        }
    }
    if (best) {
        static uint16_t logged_segment;
        if (logged_segment != segment) {
            logged_segment = segment;
            rt_log("%s at %04x found by its code (signature changed)", best->name, segment);
        }
        *load_seg = segment;
    }
    return best;
}

/* Last fallback: the module of the code file the game opened last. A function that is missing there is then
 * reported and logged like any other missing function (the next build adds it). */
static const RtModule *module_last_opened(uint16_t segment, uint16_t *load_seg)
{
    const RtModule *best = 0;
    long best_opened = 0;
    for (const RtModule *const *module = module_list(); *module; module++) {
        long opened = last_opened(*module);
        if (opened > best_opened) {
            best_opened = opened;
            best = *module;
        }
    }
    if (best) {
        rt_log("%s at %04x taken as the code file opened last", best->name, segment);
        *load_seg = segment;
    }
    return best;
}

static RtFunc resolve_far(Cpu *c, uint16_t segment, uint16_t offset, const RtModule **module, uint16_t *load_seg)
{
    *module = module_at(c, segment, load_seg);
    if (!*module) *module = module_by_code(c, segment, offset, load_seg);
    if (!*module) *module = module_last_opened(segment, load_seg);
    if (!*module) {
        rt_log("far call to %04x:%04x: signature hash %016llx matches no module", segment, offset,
               (unsigned long long)signature_hash(c, rt_linear(segment, 0), 0xFFFFFFFFu));
        if (!interpreter_enabled()) rt_fatal("far call to %04x:%04x: no recompiled module is loaded there", segment, offset);
        *load_seg = segment;
        return 0;
    }
    return find_entry(*module, (uint16_t)(segment - *load_seg), offset);
}

void rt_call_far(Cpu *c, uint16_t segment, uint16_t offset)
{
    const RtModule *module;
    uint16_t load_seg;
    RtFunc func = resolve_far(c, segment, offset, &module, &load_seg);
    const RtModule *caller = c->module;
    uint16_t caller_load = c->load_seg;
    c->cs = segment;
    c->module = module;
    c->load_seg = load_seg;
    if (func) {
        func(c);
    } else {
        interpret_missing(c, offset, "far call to");
    }
    c->module = caller;
    c->load_seg = caller_load;
}

void rt_jump_far(Cpu *c, uint16_t segment, uint16_t offset)
{
    rt_call_far(c, segment, offset);
}

void rt_start(Cpu *c, uint16_t segment, uint16_t offset)
{
    const RtModule *module;
    uint16_t load_seg;
    RtFunc func = resolve_far(c, segment, offset, &module, &load_seg);
    c->cs = segment;
    c->module = module;
    c->load_seg = load_seg;
    if (func) {
        func(c);
    } else {
        interpret_missing(c, offset, "start at");
    }
}

int rt_vector_is_recompiled(Cpu *c, uint8_t number)
{
    uint16_t segment = rd16(c, 0, (uint16_t)(number * 4 + 2));
    uint16_t offset = rd16(c, 0, (uint16_t)(number * 4));
    return segment != BIOS_SEGMENT && (segment || offset);
}

void rt_call_interrupt(Cpu *c, uint8_t number)
{
    uint16_t offset = rd16(c, 0, (uint16_t)(number * 4));
    uint16_t segment = rd16(c, 0, (uint16_t)(number * 4 + 2));
    uint16_t caller_cs = c->cs;
    push16(c, c->flags);
    push16(c, c->cs);
    push16(c, FAKE_RETURN_IP);
    c->flags &= ~(F_IF | F_TF);
    rt_call_far(c, segment, offset);
    if (c->ret_ip != FAKE_RETURN_IP || c->ret_cs != caller_cs) {
        rt_fatal("INT %02xh handler returned to %04x:%04x, expected %04x:%04x", number, c->ret_cs, c->ret_ip,
                 caller_cs, FAKE_RETURN_IP);
    }
    c->cs = caller_cs;
}

/* A near call returned to another address: the callee removed its return address and returned to an outer
 * caller (PMD command handlers do this). Nonzero = return from this C function too; the caller whose return
 * address matches continues. If no caller matches, the far call or interrupt that started the chain stops. */
int rt_check_return(Cpu *c, uint16_t expected)
{
    return c->ret_ip != expected;
}

void rt_check_far_return(Cpu *c, uint16_t segment, uint16_t offset)
{
    if (c->ret_ip != offset || c->ret_cs != segment) {
        rt_fatal("%s: far return to %04x:%04x, expected %04x:%04x", c->module->name, c->ret_cs, c->ret_ip, segment, offset);
    }
}

void rt_fell_off(Cpu *c, uint16_t address)
{
    interpret_missing(c, address, "execution continues at");
}

void rt_halt(Cpu *c)
{
    (void)c;
    rt_log("HLT executed: stop");
    exit(2);
}

void rt_fatal(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    fprintf(stderr, "rt fatal: %s\n", text);
    display_fatal(text);
    exit(3);
}

void rt_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[rt] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
}
