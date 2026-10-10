/* PC-98 BIOS (INT 18h), console (INT DCh) and driver stubs: PMD (INT 60h), PDR (INT 63h), PPSDRV (INT 64h).
 * The stubs only answer what the game asks (no sound); they are used when the recompiled drivers are not loaded. */
#include "rt.h"

#include <stdlib.h>
#include <string.h>

#define PMD_BUFFER_PARAGRAPHS 0x0400
#define PPS_BUFFER_PARAGRAPHS 0x0800

static uint16_t pmd_buffer;
static uint16_t pps_buffer;
static int logged[256];

int pc98_key_pressed(uint8_t code);

static int drivers_linked(void)
{
    for (const RtModule *const *module = rt_modules; *module; module++) {
        if (strcmp((*module)->name, "PMDB2.COM") == 0) return 1;
    }
    return 0;
}

void bios_init(Cpu *c)
{
    if (rt_vector_is_recompiled(c, 0x60) || drivers_linked()) return;
    pmd_buffer = dos_reserve(PMD_BUFFER_PARAGRAPHS);
    pps_buffer = dos_reserve(PPS_BUFFER_PARAGRAPHS);
    rt_log("bios: PMD song buffer %04x:0000, PPS buffer %04x:0000", pmd_buffer, pps_buffer);
}

static void log_once(uint8_t number, uint8_t function)
{
    int key = (number == 0x18 ? 0 : number == 0x60 ? 1 : 2) * 64 + (function & 63);
    if (!logged[key]) {
        logged[key] = 1;
        rt_log("INT %02xh AH=%02xh (first call)", number, function);
    }
}

static void bios_int18(Cpu *c)
{
    uint8_t function = c->ax.b.h;
    log_once(0x18, function);
    switch (function) {
    case 0x00: {
        uint8_t code = 0;
        while (!display_key_queued(&code, 1)) pc98_idle(c);
        c->ax.x = (uint16_t)(code << 8 | (code == 0x1C ? 0x0D : 0));
        break;
    }
    case 0x01: {
        uint8_t code;
        int queued = display_key_queued(&code, 0);
        c->bx.b.h = queued ? 1 : 0;
        if (queued) c->ax.x = (uint16_t)(code << 8 | (code == 0x1C ? 0x0D : 0));
        break;
    }
    case 0x04: {
        uint8_t bits = 0;
        for (int bit = 0; bit < 8; bit++) {
            if (pc98_key_pressed((uint8_t)(c->ax.b.l * 8 + bit))) bits |= (uint8_t)(1 << bit);
        }
        c->ax.b.h = bits;
        break;
    }
    case 0x0C:
        video_set_text_on(1);
        break;
    case 0x0D:
        video_set_text_on(0);
        break;
    case 0x40:
        video_set_graphics_on(1);
        break;
    case 0x41:
        video_set_graphics_on(0);
        break;
    case 0x42:
        video_set_graphics_mode(c->cx.b.h);
        break;
    default:
        break;
    }
}

static void pmd_int60(Cpu *c)
{
    uint8_t function = c->ax.b.h;
    log_once(0x60, function);
    switch (function) {
    case 0x06:
        c->ds = pmd_buffer;
        c->dx.x = 0;
        break;
    case 0x09:
        c->ax.b.l = 1;
        break;
    case 0x16:
        c->ax.b.l = 0xFF;
        break;
    default:
        break;
    }
}

static void pps_int64(Cpu *c)
{
    uint8_t function = c->ax.b.h;
    log_once(0x64, function);
    if (function == 0x04) {
        c->ds = pps_buffer;
        c->dx.x = 0;
    }
}

int rt_bios_int(Cpu *c, uint8_t number)
{
    switch (number) {
    case 0x18:
        bios_int18(c);
        return 1;
    case 0xDC:
        log_once(0xDC, c->ax.b.h);
        return 1;
    case 0x60:
        pmd_int60(c);
        return 1;
    case 0x63:
        log_once(0x63, c->ax.b.h);
        if (c->ax.b.h == 0x02) c->ax.b.l = 0;
        return 1;
    case 0x64:
        pps_int64(c);
        return 1;
    default:
        return 0;
    }
}
