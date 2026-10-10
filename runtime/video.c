/* PC-98 graphics model: 2 pages x 4 planes of VRAM, GRCG, EGC, analog palette, GDC scroll RAM (2 display areas), text layer,
 * frame output.
 * RUSTY_DUMP_DIR + RUSTY_DUMP_EVERY=<frames> write the displayed page as PPM files. RUSTY_TRACE_SCROLL=1 logs the
 * display areas (start byte/lines) at each scroll RAM update. */
#include "roms.h"
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLANE_SIZE 0x8000
#define FONT_ROM_NAME "FONT.ROM"

static uint8_t vram[2][4][PLANE_SIZE];
static int draw_page;
static int display_page;
static uint8_t grcg_mode;
static uint8_t grcg_tile[4];
static int grcg_tile_index;
static uint8_t palette_index;
static uint8_t palette[16][3];
static int lines_400 = 1;
static int graphics_on = 1;
static int text_on = 1;
static const uint8_t *memory;
static uint16_t gdc_command;
static int gdc_param_count;
static uint8_t scroll_ram[16];
static long dump_every = -1;
static int trace_scroll;
static const char *dump_dir;
static uint8_t frame_rgb[400][640][3];
static uint8_t font_rom[0x46800];
static int font_loaded;
static uint32_t font_version;
static uint8_t kanji_code_low;
static uint8_t kanji_code_high;
static uint8_t kanji_line;

static uint32_t font_offset(uint8_t code_high, uint8_t code_low, uint8_t line, int left)
{
    line &= 0x0F;
    if (code_high == 0) return 0x800u + (uint32_t)code_low * 16 + line;
    uint32_t row = (uint32_t)(code_high + 0x20);
    uint32_t col = code_low;
    if (row < 0x21 || row > 0x7C || col < 0x20 || col > 0x7F) return 0;
    uint32_t base = 0x1800u + ((row - 0x21) * 96 + (col - 0x20)) * 32;
    return base + (left ? 0 : 16) + line;
}

static uint32_t glyph_offset(void)
{
    return font_offset(kanji_code_high, kanji_code_low, kanji_line, (kanji_line & 0x20) != 0);
}

static int plane_of(uint32_t linear, uint16_t *offset)
{
    if (linear >= 0xA8000 && linear < 0xB0000) {
        *offset = (uint16_t)(linear - 0xA8000);
        return 0;
    }
    if (linear >= 0xB0000 && linear < 0xB8000) {
        *offset = (uint16_t)(linear - 0xB0000);
        return 1;
    }
    if (linear >= 0xB8000 && linear < 0xC0000) {
        *offset = (uint16_t)(linear - 0xB8000);
        return 2;
    }
    if (linear >= 0xE0000 && linear < 0xE8000) {
        *offset = (uint16_t)(linear - 0xE0000);
        return 3;
    }
    return -1;
}

/* EGC (enhanced graphics charger), active when the EGC mode is on (port 6Ah: 07h permits the change, 05h EGC on,
 * 04h GRCG mode) and the GRCG is enabled (7Ch bit 7). Registers 4A0h-4AEh (16-bit): plane access, pattern source and
 * lead plane, operation (ROP, light source, shifter input, register load), foreground, mask, background, bit
 * addresses and direction, bit length. VRAM accesses of 8 or 16 bits go through the shifter, a queue of 4-bit
 * pixels (one bit per plane) in drawing direction, and the raster operation of pattern, source and destination. */
static int egc_available = 1;
static int egc_permitted;
static int egc_mode;
static uint16_t egc_register[8];
static uint8_t egc_low_byte[8];
static uint16_t egc_pattern[4];
static uint16_t egc_source[4];
static uint16_t egc_last_vram[4];

#define EGC_QUEUE 8192
static uint8_t egc_queue[EGC_QUEUE];
static int egc_head;
static int egc_count;
static int egc_src_skip;
static int egc_dst_skip;
static int egc_remain;

static int egc_active(void)
{
    return egc_mode && (grcg_mode & 0x80);
}

static int egc_descending(void)
{
    return (egc_register[6] >> 12) & 1;
}

static void egc_reset_shifter(void)
{
    egc_head = 0;
    egc_count = 0;
    egc_src_skip = egc_register[6] & 0x0F;
    egc_dst_skip = (egc_register[6] >> 4) & 0x0F;
    egc_remain = (egc_register[7] & 0x0FFF) + 1;
}

/* The shifter works on 8 pixels at a time. Drawing order inside a plane byte: ascending = bit 7 first (left to right),
 * descending = bit 0 first; a 16-bit access takes the lower byte first when ascending, the upper byte first when
 * descending. egc_spread[d][v]: the 8 pixels of plane byte v in drawing order d, one per byte (bit 0). */
static uint64_t egc_spread[2][256];

static void egc_make_tables(void)
{
    for (int v = 0; v < 256; v++) {
        for (int j = 0; j < 8; j++) {
            egc_spread[0][v] |= (uint64_t)((v >> (7 - j)) & 1) << (8 * j);
            egc_spread[1][v] |= (uint64_t)((v >> j) & 1) << (8 * j);
        }
    }
}

/* The inverse of egc_spread: bit 0 of the 8 bytes of x back into one plane byte. */
static uint8_t egc_pack(uint64_t x, int descending)
{
    uint64_t magic = descending ? 0x0102040810204080ull : 0x8040201008040201ull;
    return (uint8_t)(((x & 0x0101010101010101ull) * magic) >> 56);
}

static int egc_byte_of(int group, int bits)
{
    return egc_descending() ? bits / 8 - 1 - group : group;
}

static void egc_shift_in(const uint16_t data[4], int bits)
{
    int descending = egc_descending();
    for (int group = 0; group < bits / 8; group++) {
        int shift = 8 * egc_byte_of(group, bits);
        uint64_t pixels = 0;
        for (int p = 0; p < 4; p++) pixels |= egc_spread[descending][(uint8_t)(data[p] >> shift)] << p;
        int first = egc_src_skip < 8 ? egc_src_skip : 8;
        egc_src_skip -= first;
        for (int j = first; j < 8 && egc_count < EGC_QUEUE; j++)
            egc_queue[(egc_head + egc_count++) & (EGC_QUEUE - 1)] = (uint8_t)(pixels >> (8 * j));
    }
}

/* Take pixels for one access; mask gets the bits the shifter provides (0 when it has not enough pixels yet). */
static void egc_shift_out(uint16_t out[4], uint16_t *mask, int bits)
{
    int skip = egc_dst_skip < bits ? egc_dst_skip : bits;
    int wanted = bits - skip;
    if (wanted > egc_remain) wanted = egc_remain;
    for (int p = 0; p < 4; p++) out[p] = 0;
    *mask = 0;
    if (wanted > 0 && egc_count < wanted) return;
    egc_dst_skip -= skip;
    /* Pixels by drawing position; bit 4 marks the positions the shifter fills (it becomes the mask). */
    uint8_t slots[16] = {0};
    for (int i = skip; i < skip + wanted; i++) {
        slots[i] = (uint8_t)(egc_queue[egc_head] | 0x10);
        egc_head = (egc_head + 1) & (EGC_QUEUE - 1);
    }
    egc_count -= wanted;
    int descending = egc_descending();
    for (int group = 0; group < bits / 8; group++) {
        uint64_t x = 0;
        for (int j = 0; j < 8; j++) x |= (uint64_t)slots[8 * group + j] << (8 * j);
        int shift = 8 * egc_byte_of(group, bits);
        for (int p = 0; p < 4; p++) out[p] |= (uint16_t)(egc_pack(x >> p, descending) << shift);
        *mask |= (uint16_t)(egc_pack(x >> 4, descending) << shift);
    }
    egc_remain -= wanted;
    if (egc_remain <= 0) egc_reset_shifter();
}

static uint16_t egc_color_planes(uint8_t color, int p)
{
    return (color >> p) & 1 ? 0xFFFF : 0x0000;
}

static void egc_read_planes(uint16_t offset, int bits, uint16_t data[4])
{
    for (int p = 0; p < 4; p++) {
        data[p] = vram[draw_page][p][offset];
        if (bits == 16) data[p] |= (uint16_t)(vram[draw_page][p][(offset + 1) % PLANE_SIZE] << 8);
    }
}

/* Byte accesses use the byte lane of their address in the 16-bit registers (pattern, colors). */
static uint16_t egc_lane(uint16_t value, uint16_t offset, int bits)
{
    if (bits == 16) return value;
    return (offset & 1) ? (uint16_t)(value >> 8) : (uint16_t)(value & 0xFF);
}

static uint16_t egc_read(uint16_t offset, int bits, int plane)
{
    egc_read_planes(offset, bits, egc_last_vram);
    uint16_t operation = egc_register[2];
    int shift_from_cpu = (operation >> 10) & 1;
    if (!shift_from_cpu) {
        uint16_t mask;
        egc_shift_in(egc_last_vram, bits);
        egc_shift_out(egc_source, &mask, bits);
    }
    if ((operation >> 8) & 1) {
        for (int p = 0; p < 4; p++) {
            if (bits == 16) {
                egc_pattern[p] = egc_last_vram[p];
            } else if (offset & 1) {
                egc_pattern[p] = (uint16_t)((egc_pattern[p] & 0x00FF) | (egc_last_vram[p] << 8));
            } else {
                egc_pattern[p] = (uint16_t)((egc_pattern[p] & 0xFF00) | egc_last_vram[p]);
            }
        }
    }
    if (!((operation >> 13) & 1)) {
        int lead = (egc_register[1] >> 8) & 3;
        return shift_from_cpu ? egc_last_vram[lead] : egc_source[lead];
    }
    return egc_last_vram[plane];
}

/* Bit k of the ROP is the result for source = k bit 2, destination = k bit 1, pattern = k bit 0: a selection tree
 * over the 8 constant results (results[k] = FFFFh or 0), for 16 pixels at once. */
static uint16_t egc_raster_operation(const uint16_t results[8], uint16_t source, uint16_t destination, uint16_t pattern)
{
    uint16_t level[4];
    for (int k = 0; k < 4; k++) level[k] = (uint16_t)((pattern & results[2 * k + 1]) | (~pattern & results[2 * k]));
    for (int k = 0; k < 2; k++) level[k] = (uint16_t)((destination & level[2 * k + 1]) | (~destination & level[2 * k]));
    return (uint16_t)((source & level[1]) | (~source & level[0]));
}

static void egc_write(uint16_t offset, int bits, uint16_t value)
{
    uint16_t operation = egc_register[2];
    int light = (operation >> 11) & 3;
    int color_source = (egc_register[1] >> 13) & 3;
    uint16_t full = bits == 16 ? 0xFFFF : 0x00FF;
    uint16_t mask = egc_lane(egc_register[4], offset, bits) & full;
    if ((operation >> 9) & 1) {
        uint16_t now[4];
        egc_read_planes(offset, bits, now);
        for (int p = 0; p < 4; p++) egc_pattern[p] = bits == 16 ? now[p] : (uint16_t)(now[p] * 0x0101);
    }
    uint16_t data[4];
    if (light == 0) {
        uint16_t v = bits == 16 ? value : (uint16_t)(value & 0xFF);
        for (int p = 0; p < 4; p++) data[p] = v;
    } else {
        if ((operation >> 10) & 1) {
            uint16_t cpu[4] = {value, value, value, value};
            uint16_t source_mask;
            egc_shift_in(cpu, bits);
            egc_shift_out(egc_source, &source_mask, bits);
            mask &= source_mask;
        }
        /* The destination and the pattern only when the operation uses them (the game draws with F0h = source). */
        uint8_t rop = (uint8_t)operation;
        int uses_destination = light != 2 && (((rop >> 2) ^ rop) & 0x33) != 0;
        int uses_pattern = light == 2 ? color_source != 0 : (((rop >> 1) ^ rop) & 0x55) != 0;
        uint16_t dest[4] = {0, 0, 0, 0};
        if (uses_destination) egc_read_planes(offset, bits, dest);
        uint16_t results[8];
        for (int k = 0; k < 8; k++) results[k] = (rop >> k) & 1 ? 0xFFFF : 0x0000;
        for (int p = 0; p < 4; p++) {
            uint16_t pattern = 0;
            if (uses_pattern && color_source == 1) {
                pattern = egc_color_planes((uint8_t)egc_register[5], p);
            } else if (uses_pattern && color_source == 2) {
                pattern = egc_color_planes((uint8_t)egc_register[3], p);
            } else if (uses_pattern) {
                pattern = egc_lane(egc_pattern[p], offset, bits);
            }
            if (light == 2) {
                data[p] = color_source == 0 ? egc_source[p] : pattern;
                continue;
            }
            data[p] = egc_raster_operation(results, egc_source[p], dest[p], pattern);
        }
    }
    for (int p = 0; p < 4; p++) {
        if ((egc_register[0] >> p) & 1) continue;
        for (int b = 0; b < bits / 8; b++) {
            uint8_t m = (uint8_t)(mask >> (8 * b));
            if (!m) continue;
            uint8_t *cell = &vram[draw_page][p][(offset + b) % PLANE_SIZE];
            *cell = (uint8_t)((*cell & ~m) | ((data[p] >> (8 * b)) & m));
        }
    }
}

static void egc_register_write(int index, uint16_t value)
{
    if (index == 4 && ((egc_register[1] >> 13) & 3) != 0) return;
    egc_register[index] = value;
    if (index == 6 || index == 7) egc_reset_shifter();
}

uint8_t rt_mmio_read8(Cpu *c, uint32_t linear)
{
    rt_changes++;
    uint16_t offset;
    int plane = plane_of(linear, &offset);
    if (plane < 0) return c->mem[linear];
    if (egc_active()) return (uint8_t)egc_read(offset, 8, plane);
    if ((grcg_mode & 0x80) && !(grcg_mode & 0x40)) {
        uint8_t result = 0xFF;
        for (int p = 0; p < 4; p++) {
            if (!(grcg_mode & (1 << p))) result &= (uint8_t)~(vram[draw_page][p][offset] ^ grcg_tile[p]);
        }
        return result;
    }
    return vram[draw_page][plane][offset];
}

void rt_mmio_write8(Cpu *c, uint32_t linear, uint8_t value)
{
    rt_changes++;
    uint16_t offset;
    int plane = plane_of(linear, &offset);
    if (plane < 0) {
        c->mem[linear] = value;
        return;
    }
    if (egc_active()) {
        egc_write(offset, 8, value);
        return;
    }
    if (grcg_mode & 0x80) {
        for (int p = 0; p < 4; p++) {
            if (grcg_mode & (1 << p)) continue;
            uint8_t *cell = &vram[draw_page][p][offset];
            if (grcg_mode & 0x40) {
                *cell = (uint8_t)((grcg_tile[p] & value) | (*cell & ~value));
            } else {
                *cell = grcg_tile[p];
            }
        }
        return;
    }
    vram[draw_page][plane][offset] = value;
}

/* Word accesses to VRAM: the EGC works on 16-bit units; otherwise two byte accesses. */
uint16_t rt_mmio_read16(Cpu *c, uint32_t linear)
{
    rt_changes++;
    uint16_t offset;
    int plane = plane_of(linear, &offset);
    if (plane >= 0 && egc_active() && !(offset & 1)) return egc_read(offset, 16, plane);
    return (uint16_t)(rt_mmio_read8(c, linear) | (rt_mmio_read8(c, (linear + 1) & 0xFFFFF) << 8));
}

void rt_mmio_write16(Cpu *c, uint32_t linear, uint16_t value)
{
    rt_changes++;
    uint16_t offset;
    int plane = plane_of(linear, &offset);
    if (plane >= 0 && egc_active() && !(offset & 1)) {
        egc_write(offset, 16, value);
        return;
    }
    rt_mmio_write8(c, linear, (uint8_t)value);
    rt_mmio_write8(c, (linear + 1) & 0xFFFFF, (uint8_t)(value >> 8));
}

/* The kanji font ROM (FONT.ROM, as used by Neko Project II): RUSTY_FONT_ROM or the ROM folders (roms.c). */
static void load_font_rom(void)
{
    char path[1024];
    int found = rom_find("RUSTY_FONT_ROM", FONT_ROM_NAME, 0, path, sizeof(path));
    if (found == ROM_DISABLED) return;
    if (found == ROM_MISSING) {
        rt_log("video: kanji ROM %s (%zu bytes) not found in: %s. Put it in one of these folders or set "
               "RUSTY_FONT_ROM=<file>. Without it all text is empty",
               FONT_ROM_NAME, sizeof(font_rom), path);
        display_message(FONT_ROM_NAME " (PC-98 kanji font ROM, 288768 bytes) not found: all text is empty. Put it "
                        "next to the game image, or in roms/ next to the program (see README.txt).",
                        10);
        return;
    }
    FILE *font = fopen(path, "rb");
    long size = 0;
    if (font) {
        size = (long)fread(font_rom, 1, sizeof(font_rom), font);
        if (fgetc(font) != EOF) size++;
        fclose(font);
    }
    font_loaded = size == (long)sizeof(font_rom);
    if (font_loaded) {
        rt_log("video: kanji ROM loaded (%s)", path);
        return;
    }
    memset(font_rom, 0, sizeof(font_rom));
    rt_log("video: kanji ROM %s: %s, expected %zu bytes. Without it all text is empty", path,
           font ? "wrong size" : "cannot read", sizeof(font_rom));
    display_message(font ? FONT_ROM_NAME " has the wrong size (288768 bytes expected): all text is empty."
                         : FONT_ROM_NAME " cannot be read: all text is empty.",
                    10);
}

void video_init(const uint8_t *cpu_memory)
{
    memory = cpu_memory;
    egc_make_tables();
    load_font_rom();
    const char *every = getenv("RUSTY_DUMP_EVERY");
    dump_dir = getenv("RUSTY_DUMP_DIR");
    trace_scroll = getenv("RUSTY_TRACE_SCROLL") != 0;
    if (dump_dir && every) dump_every = atol(every);
    for (int i = 0; i < 16; i++) {
        palette[i][0] = (uint8_t)((i & 2) ? 15 : 0);
        palette[i][1] = (uint8_t)((i & 4) ? 15 : 0);
        palette[i][2] = (uint8_t)((i & 1) ? 15 : 0);
    }
}

void video_set_graphics_mode(uint8_t ch)
{
    lines_400 = (ch & 0xC0) == 0xC0;
}

void video_set_egc_available(int available)
{
    egc_available = available;
}

void video_set_graphics_on(int on)
{
    graphics_on = on;
}

void video_set_text_on(int on)
{
    text_on = on;
}

static uint32_t area_start(int area)
{
    const uint8_t *p = &scroll_ram[area * 4];
    return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)(p[2] & 0x03) << 16)) * 2;
}

static int area_lines(int area)
{
    const uint8_t *p = &scroll_ram[area * 4];
    return ((p[2] >> 4) & 0x0F) | ((p[3] & 0x3F) << 4);
}

int video_out8(uint16_t port, uint8_t value)
{
    switch (port) {
    case 0xA2:
        gdc_command = value;
        gdc_param_count = 0;
        if (value == 0x0D || value == 0x6B) graphics_on = 1;
        if (value == 0x0C) graphics_on = 0;
        return 1;
    case 0xA0:
        if ((gdc_command & 0xF0) == 0x70) {
            int index = (gdc_command & 0x0F) + gdc_param_count;
            if (index < 16) scroll_ram[index] = value;
            if (trace_scroll && index == 7) {
                rt_log("scroll frame %ld: %u/%d %u/%d", pc98_frame(), area_start(0), area_lines(0), area_start(1),
                       area_lines(1));
            }
        }
        gdc_param_count++;
        return 1;
    case 0xA4:
        display_page = value & 1;
        return 1;
    case 0xA6:
        draw_page = value & 1;
        return 1;
    case 0x7C:
        grcg_mode = value;
        grcg_tile_index = 0;
        return 1;
    case 0x7E:
        egc_pattern[grcg_tile_index] = (uint16_t)(value * 0x0101);
        grcg_tile[grcg_tile_index] = value;
        grcg_tile_index = (grcg_tile_index + 1) & 3;
        return 1;
    case 0xA8:
        palette_index = value & 0x0F;
        return 1;
    case 0xAA:
        palette[palette_index][1] = value & 0x0F;
        return 1;
    case 0xAC:
        palette[palette_index][0] = value & 0x0F;
        return 1;
    case 0xAE:
        palette[palette_index][2] = value & 0x0F;
        return 1;
    case 0xA1:
        kanji_code_low = value;
        return 1;
    case 0xA3:
        kanji_code_high = value;
        return 1;
    case 0xA5:
        kanji_line = value;
        return 1;
    case 0xA9: {
        /* Only the user-defined characters (JIS rows 76h/77h) are RAM; writes to other codes are ignored, as on
         * the real character generator (and in DOSBox-X). */
        uint32_t offset = glyph_offset();
        if (offset && (kanji_code_high & 0x7E) == 0x56 && kanji_code_low != 0 && font_rom[offset] != value) {
            font_rom[offset] = value;
            font_version++;
        }
        return 1;
    }
    case 0x6A:
        if (value == 0x07) egc_permitted = 1;
        if (value == 0x06) egc_permitted = 0;
        if (egc_available && egc_permitted && (value == 0x05 || value == 0x04)) egc_mode = value == 0x05;
        return 1;
    case 0x68:
    case 0x6C:
    case 0x6E:
        return 1;
    default:
        if (port >= 0x4A0 && port <= 0x4AF) {
            int index = (port - 0x4A0) >> 1;
            if (!(port & 1)) {
                egc_low_byte[index] = value;
            } else if (egc_mode) {
                egc_register_write(index, (uint16_t)(egc_low_byte[index] | (value << 8)));
            }
            return 1;
        }
        return 0;
    }
}

int video_in8(uint16_t port, uint8_t *value)
{
    if (port == 0xA9) {
        if (getenv("RUSTY_TRACE_FONT")) {
            static uint32_t seen[512];
            static int count;
            uint32_t key = (uint32_t)kanji_code_high << 8 | kanji_code_low;
            int known = 0;
            for (int i = 0; i < count; i++) known |= seen[i] == key;
            if (!known && count < 512) {
                seen[count++] = key;
                fprintf(stderr, "[font] read A3=%02x A1=%02x line=%02x frame %ld\n", kanji_code_high, kanji_code_low,
                        kanji_line, pc98_frame());
            }
        }
        uint32_t offset = glyph_offset();
        *value = offset ? font_rom[offset] : 0;
        return 1;
    }
    return 0;
}

/* Text layer over the graphics: 80x25 cells of 8x16 pixels. Codes at A0000h (ANK: low byte = character, high byte
 * 0; kanji: low byte = JIS first byte - 20h, bit 7 set in the right cell, high byte = JIS second byte), attributes
 * at A2000h (bit 0 visible, bit 2 reverse, bits 5-7 = blue, red, green). */
static void render_text(void)
{
    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            uint32_t cell = (uint32_t)(row * 80 + col) * 2;
            uint8_t code_low = memory[0xA0000 + cell];
            uint8_t code_high = memory[0xA0001 + cell];
            uint8_t attribute = memory[0xA2000 + cell];
            if (!(attribute & 0x01)) continue;
            uint8_t rgb[3] = {(attribute & 0x40) ? 255 : 0, (attribute & 0x80) ? 255 : 0, (attribute & 0x20) ? 255 : 0};
            for (int line = 0; line < 16; line++) {
                uint32_t offset = code_high == 0 ? font_offset(0, code_low, (uint8_t)line, 1)
                                                 : font_offset((uint8_t)(code_low & 0x7F), code_high, (uint8_t)line,
                                                               !(code_low & 0x80));
                uint8_t bits = offset ? font_rom[offset] : 0;
                if (attribute & 0x04) bits = (uint8_t)~bits;
                for (int bit = 0; bit < 8; bit++) {
                    if (!(bits & (0x80 >> bit))) continue;
                    memcpy(frame_rgb[row * 16 + line][col * 8 + bit], rgb, 3);
                }
            }
        }
    }
}

const uint8_t *video_render(void)
{
    int page = display_page;
    int rows = lines_400 ? 400 : 200;
    int first_lines = area_lines(0);
    if (first_lines == 0) first_lines = rows;
    for (int y = 0; y < 400; y++) {
        int source_row = lines_400 ? y : y / 2;
        uint32_t row_offset;
        if (source_row < first_lines) {
            row_offset = (area_start(0) + (uint32_t)source_row * 80) % PLANE_SIZE;
        } else {
            row_offset = (area_start(1) + (uint32_t)(source_row - first_lines) * 80) % PLANE_SIZE;
        }
        for (int xb = 0; xb < 80; xb++) {
            uint32_t o = (row_offset + (uint32_t)xb) % PLANE_SIZE;
            uint8_t b = vram[page][0][o], r = vram[page][1][o], g = vram[page][2][o], i = vram[page][3][o];
            for (int bit = 0; bit < 8; bit++) {
                int shift = 7 - bit;
                int color = ((b >> shift) & 1) | (((r >> shift) & 1) << 1) | (((g >> shift) & 1) << 2) | (((i >> shift) & 1) << 3);
                uint8_t *pixel = frame_rgb[y][xb * 8 + bit];
                if (!graphics_on || source_row >= rows) {
                    pixel[0] = pixel[1] = pixel[2] = 0;
                } else {
                    pixel[0] = (uint8_t)(palette[color][0] * 17);
                    pixel[1] = (uint8_t)(palette[color][1] * 17);
                    pixel[2] = (uint8_t)(palette[color][2] * 17);
                }
            }
        }
    }
    if (text_on) render_text();
    return &frame_rgb[0][0][0];
}

void video_state(VideoState *state)
{
    state->planes = &vram[display_page][0][0];
    state->text = &memory[0xA0000];
    state->font = font_rom;
    state->font_size = sizeof(font_rom);
    state->font_version = font_version;
    memcpy(state->palette, palette, sizeof(palette));
    state->area_start[0] = area_start(0);
    state->area_start[1] = area_start(1);
    state->area0_lines = area_lines(0);
    state->lines_400 = lines_400;
    state->graphics_on = graphics_on;
    state->text_on = text_on;
}

void video_frame(long frame)
{
    if (dump_every <= 0 || frame % dump_every != 0) return;
    video_render();
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%06ld.ppm", dump_dir, frame);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P6\n640 400\n255\n");
    fwrite(frame_rgb, 1, sizeof(frame_rgb), file);
    fclose(file);
}

/* Snapshot: VRAM, display and GDC state, GRCG, EGC, palette, and the character generator (the game writes its own
 * glyphs into the font RAM). */
void video_snapshot(StateStream *s)
{
    STATE(s, vram);
    STATE(s, draw_page);
    STATE(s, display_page);
    STATE(s, grcg_mode);
    STATE(s, grcg_tile);
    STATE(s, grcg_tile_index);
    STATE(s, palette_index);
    STATE(s, palette);
    STATE(s, lines_400);
    STATE(s, graphics_on);
    STATE(s, text_on);
    STATE(s, gdc_command);
    STATE(s, gdc_param_count);
    STATE(s, scroll_ram);
    STATE(s, font_rom);
    STATE(s, kanji_code_low);
    STATE(s, kanji_code_high);
    STATE(s, kanji_line);
    STATE(s, egc_permitted);
    STATE(s, egc_mode);
    STATE(s, egc_register);
    STATE(s, egc_low_byte);
    STATE(s, egc_pattern);
    STATE(s, egc_source);
    STATE(s, egc_last_vram);
    STATE(s, egc_queue);
    STATE(s, egc_head);
    STATE(s, egc_count);
    STATE(s, egc_src_skip);
    STATE(s, egc_dst_skip);
    STATE(s, egc_remain);
    if (state_loading(s)) font_version++;
}
