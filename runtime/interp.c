/* Fallback interpreter for code that is not recompiled (8086/80186 real mode, the instruction set of the game).
 * rt_interpret runs the original instructions from CS:IP on the same Cpu and memory, with the same ALU/flag helpers
 * as the generated code, the instruction count (time) and interrupt checks at backward jumps and after STI/POPF.
 * It returns like a recompiled function: when a RET/RETF/IRET lifts SP above the entry SP (c->ret_ip/ret_cs hold
 * the return address). Calls go to recompiled functions when the target has one, otherwise they are interpreted
 * too. Unknown call targets are logged (build/recomp/missing.txt), so the next build recompiles them. */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint16_t ip;
    int segment_override;
    uint16_t override;
    int rep;
    uint8_t modrm;
    int mod, reg, rm;
    uint16_t ea_seg, ea_off;
} Decode;

static uint8_t fetch8(Cpu *c, Decode *d)
{
    return rd8(c, c->cs, d->ip++);
}

static uint16_t fetch16(Cpu *c, Decode *d)
{
    uint16_t value = rd16(c, c->cs, d->ip);
    d->ip += 2;
    return value;
}

static uint16_t *reg16(Cpu *c, int index)
{
    switch (index & 7) {
    case 0: return &c->ax.x;
    case 1: return &c->cx.x;
    case 2: return &c->dx.x;
    case 3: return &c->bx.x;
    case 4: return &c->sp;
    case 5: return &c->bp;
    case 6: return &c->si;
    default: return &c->di;
    }
}

static uint8_t *reg8(Cpu *c, int index)
{
    switch (index & 7) {
    case 0: return &c->ax.b.l;
    case 1: return &c->cx.b.l;
    case 2: return &c->dx.b.l;
    case 3: return &c->bx.b.l;
    case 4: return &c->ax.b.h;
    case 5: return &c->cx.b.h;
    case 6: return &c->dx.b.h;
    default: return &c->bx.b.h;
    }
}

static uint16_t *sreg(Cpu *c, int index)
{
    switch (index & 3) {
    case 0: return &c->es;
    case 1: return &c->cs;
    case 2: return &c->ss;
    default: return &c->ds;
    }
}

static uint16_t data_segment(Cpu *c, Decode *d)
{
    return d->segment_override ? d->override : c->ds;
}

static void decode_modrm(Cpu *c, Decode *d)
{
    d->modrm = fetch8(c, d);
    d->mod = d->modrm >> 6;
    d->reg = (d->modrm >> 3) & 7;
    d->rm = d->modrm & 7;
    if (d->mod == 3) return;
    uint16_t offset = 0;
    int stack = 0;
    switch (d->rm) {
    case 0: offset = (uint16_t)(c->bx.x + c->si); break;
    case 1: offset = (uint16_t)(c->bx.x + c->di); break;
    case 2: offset = (uint16_t)(c->bp + c->si); stack = 1; break;
    case 3: offset = (uint16_t)(c->bp + c->di); stack = 1; break;
    case 4: offset = c->si; break;
    case 5: offset = c->di; break;
    case 6:
        if (d->mod == 0) {
            offset = fetch16(c, d);
        } else {
            offset = c->bp;
            stack = 1;
        }
        break;
    default: offset = c->bx.x; break;
    }
    if (d->mod == 1) offset = (uint16_t)(offset + (int8_t)fetch8(c, d));
    if (d->mod == 2) offset = (uint16_t)(offset + fetch16(c, d));
    d->ea_off = offset;
    d->ea_seg = d->segment_override ? d->override : (stack ? c->ss : c->ds);
}

static uint8_t get_rm8(Cpu *c, Decode *d)
{
    return d->mod == 3 ? *reg8(c, d->rm) : rd8(c, d->ea_seg, d->ea_off);
}

static uint16_t get_rm16(Cpu *c, Decode *d)
{
    return d->mod == 3 ? *reg16(c, d->rm) : rd16(c, d->ea_seg, d->ea_off);
}

static void set_rm8(Cpu *c, Decode *d, uint8_t value)
{
    if (d->mod == 3) {
        *reg8(c, d->rm) = value;
    } else {
        wr8(c, d->ea_seg, d->ea_off, value);
    }
}

static void set_rm16(Cpu *c, Decode *d, uint16_t value)
{
    if (d->mod == 3) {
        *reg16(c, d->rm) = value;
    } else {
        wr16(c, d->ea_seg, d->ea_off, value);
    }
}

static uint8_t alu8(Cpu *c, int op, uint8_t a, uint8_t b)
{
    switch (op) {
    case 0: return alu_add8(c, a, b);
    case 1: return alu_or8(c, a, b);
    case 2: return alu_adc8(c, a, b);
    case 3: return alu_sbb8(c, a, b);
    case 4: return alu_and8(c, a, b);
    case 5: return alu_sub8(c, a, b);
    case 6: return alu_xor8(c, a, b);
    default: alu_sub8(c, a, b); return a;
    }
}

static uint16_t alu16(Cpu *c, int op, uint16_t a, uint16_t b)
{
    switch (op) {
    case 0: return alu_add16(c, a, b);
    case 1: return alu_or16(c, a, b);
    case 2: return alu_adc16(c, a, b);
    case 3: return alu_sbb16(c, a, b);
    case 4: return alu_and16(c, a, b);
    case 5: return alu_sub16(c, a, b);
    case 6: return alu_xor16(c, a, b);
    default: alu_sub16(c, a, b); return a;
    }
}

static uint8_t shift8(Cpu *c, int op, uint8_t a, uint8_t count)
{
    switch (op) {
    case 0: return alu_rol8(c, a, count);
    case 1: return alu_ror8(c, a, count);
    case 2: return alu_rcl8(c, a, count);
    case 3: return alu_rcr8(c, a, count);
    case 5: return alu_shr8(c, a, count);
    case 7: return alu_sar8(c, a, count);
    default: return alu_shl8(c, a, count);
    }
}

static uint16_t shift16(Cpu *c, int op, uint16_t a, uint8_t count)
{
    switch (op) {
    case 0: return alu_rol16(c, a, count);
    case 1: return alu_ror16(c, a, count);
    case 2: return alu_rcl16(c, a, count);
    case 3: return alu_rcr16(c, a, count);
    case 5: return alu_shr16(c, a, count);
    case 7: return alu_sar16(c, a, count);
    default: return alu_shl16(c, a, count);
    }
}

static int condition(Cpu *c, int code)
{
    int result;
    switch (code >> 1) {
    case 0: result = FLAG_OF(c); break;
    case 1: result = FLAG_CF(c); break;
    case 2: result = FLAG_ZF(c); break;
    case 3: result = FLAG_CF(c) || FLAG_ZF(c); break;
    case 4: result = FLAG_SF(c); break;
    case 5: result = FLAG_PF(c); break;
    case 6: result = FLAG_SF(c) != FLAG_OF(c); break;
    default: result = FLAG_ZF(c) || FLAG_SF(c) != FLAG_OF(c); break;
    }
    return (code & 1) ? !result : result;
}

/* One string instruction step; returns 0 when a REPE/REPNE condition ends the loop. */
static void string_op(Cpu *c, Decode *d, uint8_t op)
{
    int wide = op & 1;
    int step = (c->flags & F_DF) ? -(wide ? 2 : 1) : (wide ? 2 : 1);
    int compare = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    for (;;) {
        if (d->rep && c->cx.x == 0) return;
        if (d->rep) c->icount++;
        uint16_t source_seg = data_segment(c, d);
        switch (op) {
        case 0xA4: wr8(c, c->es, c->di, rd8(c, source_seg, c->si)); c->si += step; c->di += step; break;
        case 0xA5: wr16(c, c->es, c->di, rd16(c, source_seg, c->si)); c->si += step; c->di += step; break;
        case 0xA6: alu_sub8(c, rd8(c, source_seg, c->si), rd8(c, c->es, c->di)); c->si += step; c->di += step; break;
        case 0xA7: alu_sub16(c, rd16(c, source_seg, c->si), rd16(c, c->es, c->di)); c->si += step; c->di += step; break;
        case 0xAA: wr8(c, c->es, c->di, c->ax.b.l); c->di += step; break;
        case 0xAB: wr16(c, c->es, c->di, c->ax.x); c->di += step; break;
        case 0xAC: c->ax.b.l = rd8(c, source_seg, c->si); c->si += step; break;
        case 0xAD: c->ax.x = rd16(c, source_seg, c->si); c->si += step; break;
        case 0xAE: alu_sub8(c, c->ax.b.l, rd8(c, c->es, c->di)); c->di += step; break;
        case 0xAF: alu_sub16(c, c->ax.x, rd16(c, c->es, c->di)); c->di += step; break;
        case 0x6E: rt_out8(c, c->dx.x, rd8(c, source_seg, c->si)); c->si += step; break;
        case 0x6F: rt_out16(c, c->dx.x, rd16(c, source_seg, c->si)); c->si += step; break;
        case 0x6C: wr8(c, c->es, c->di, rt_in8(c, c->dx.x)); c->di += step; break;
        default: wr16(c, c->es, c->di, rt_in16(c, c->dx.x)); c->di += step; break;
        }
        if (!d->rep) return;
        c->cx.x--;
        if (compare) {
            if (d->rep == 0xF3 && !FLAG_ZF(c)) return;
            if (d->rep == 0xF2 && FLAG_ZF(c)) return;
        }
    }
}

static void op_daa(Cpu *c, int subtract)
{
    uint8_t old = c->ax.b.l;
    int carry = FLAG_CF(c);
    uint16_t flags = c->flags & ~(F_CF | F_AF);
    if ((old & 0x0F) > 9 || (c->flags & F_AF)) {
        c->ax.b.l = (uint8_t)(subtract ? c->ax.b.l - 6 : c->ax.b.l + 6);
        flags |= F_AF;
        if (subtract ? old < 6 : old > 0xF9) carry = 1;
    }
    if (old > 0x99 || carry) {
        c->ax.b.l = (uint8_t)(subtract ? c->ax.b.l - 0x60 : c->ax.b.l + 0x60);
        flags |= F_CF;
    }
    c->flags = flags;
    uint8_t v = c->ax.b.l;
    c->flags &= ~(F_ZF | F_SF | F_PF);
    if (!v) c->flags |= F_ZF;
    if (v & 0x80) c->flags |= F_SF;
    if (!__builtin_parity(v)) c->flags |= F_PF;
}

static void set_szp8_flags(Cpu *c, uint8_t v)
{
    c->flags &= ~(F_ZF | F_SF | F_PF);
    if (!v) c->flags |= F_ZF;
    if (v & 0x80) c->flags |= F_SF;
    if (!__builtin_parity(v)) c->flags |= F_PF;
}

static void unsupported(Cpu *c, uint16_t ip, uint8_t op)
{
    rt_fatal("interpreter: unsupported instruction %02x at %04x:%04x", op, c->cs, ip);
}

/* A call from interpreted code: to the recompiled function when there is one, else interpreted. */
static void interpreted_call_near(Cpu *c, uint16_t target)
{
    RtFunc func = c->module ? rt_find_function(c->module, (uint16_t)(c->cs - c->load_seg), target) : 0;
    if (func) {
        func(c);
    } else {
        rt_interpret(c, target);
    }
}

/* The interpreted function has returned when SP is above its entry SP on the same stack (a handler can switch to
 * its own stack, such as PMD's). */
#define LEFT_FRAME() (c->ss == entry_ss && c->sp > entry_sp)

void rt_interpret(Cpu *c, uint16_t ip)
{
    uint16_t entry_sp = c->sp;
    uint16_t entry_ss = c->ss;
    uint16_t entry_cs = c->cs;
    const RtModule *entry_module = c->module;
    uint16_t entry_load = c->load_seg;
    for (;;) {
        Decode d = {.ip = ip};
        uint16_t start = ip;
        uint8_t op;
        for (;;) {
            op = fetch8(c, &d);
            if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) {
                d.segment_override = 1;
                d.override = *sreg(c, (op >> 3) & 3);
            } else if (op == 0xF2 || op == 0xF3) {
                d.rep = op;
            } else if (op != 0xF0) {
                break;
            }
        }
        c->icount++;
        uint16_t next;
        switch (op) {
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
            decode_modrm(c, &d);
            {
                uint8_t r = alu8(c, op >> 3, get_rm8(c, &d), *reg8(c, d.reg));
                if (op != 0x38) set_rm8(c, &d, r);
            }
            break;
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39:
            decode_modrm(c, &d);
            {
                uint16_t r = alu16(c, op >> 3, get_rm16(c, &d), *reg16(c, d.reg));
                if (op != 0x39) set_rm16(c, &d, r);
            }
            break;
        case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A:
            decode_modrm(c, &d);
            {
                uint8_t r = alu8(c, op >> 3, *reg8(c, d.reg), get_rm8(c, &d));
                if (op != 0x3A) *reg8(c, d.reg) = r;
            }
            break;
        case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B:
            decode_modrm(c, &d);
            {
                uint16_t r = alu16(c, op >> 3, *reg16(c, d.reg), get_rm16(c, &d));
                if (op != 0x3B) *reg16(c, d.reg) = r;
            }
            break;
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: {
            uint8_t r = alu8(c, op >> 3, c->ax.b.l, fetch8(c, &d));
            if (op != 0x3C) c->ax.b.l = r;
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            uint16_t r = alu16(c, op >> 3, c->ax.x, fetch16(c, &d));
            if (op != 0x3D) c->ax.x = r;
            break;
        }
        case 0x06: case 0x0E: case 0x16: case 0x1E:
            push16(c, *sreg(c, op >> 3));
            break;
        case 0x07: case 0x17: case 0x1F:
            *sreg(c, op >> 3) = pop16(c);
            break;
        case 0x27: op_daa(c, 0); break;
        case 0x2F: op_daa(c, 1); break;
        case 0x37: case 0x3F: {
            int subtract = op == 0x3F;
            if ((c->ax.b.l & 0x0F) > 9 || (c->flags & F_AF)) {
                c->ax.b.l = (uint8_t)(subtract ? c->ax.b.l - 6 : c->ax.b.l + 6);
                c->ax.b.h = (uint8_t)(subtract ? c->ax.b.h - 1 : c->ax.b.h + 1);
                c->flags |= F_AF | F_CF;
            } else {
                c->flags &= ~(F_AF | F_CF);
            }
            c->ax.b.l &= 0x0F;
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            *reg16(c, op) = alu_inc16(c, *reg16(c, op));
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            *reg16(c, op) = alu_dec16(c, *reg16(c, op));
            break;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            push16(c, *reg16(c, op));
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            *reg16(c, op) = pop16(c);
            break;
        case 0x60: op_pusha(c); break;
        case 0x61: op_popa(c); break;
        case 0x68: push16(c, fetch16(c, &d)); break;
        case 0x6A: push16(c, (uint16_t)(int8_t)fetch8(c, &d)); break;
        case 0x69: case 0x6B:
            decode_modrm(c, &d);
            {
                uint16_t a = get_rm16(c, &d);
                uint16_t b = op == 0x69 ? fetch16(c, &d) : (uint16_t)(int8_t)fetch8(c, &d);
                *reg16(c, d.reg) = op_imul_trunc16(c, a, b);
            }
            break;
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            string_op(c, &d, op);
            break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int8_t displacement = (int8_t)fetch8(c, &d);
            if (condition(c, op & 0x0F)) {
                d.ip = (uint16_t)(d.ip + displacement);
                if (displacement < 0) rt_poll_loop(c, d.ip);
            }
            break;
        }
        case 0x80: case 0x82: case 0x83: case 0x81:
            decode_modrm(c, &d);
            if (op == 0x81) {
                uint16_t imm = fetch16(c, &d);
                uint16_t r = alu16(c, d.reg, get_rm16(c, &d), imm);
                if (d.reg != 7) set_rm16(c, &d, r);
            } else if (op == 0x83) {
                uint16_t imm = (uint16_t)(int8_t)fetch8(c, &d);
                uint16_t r = alu16(c, d.reg, get_rm16(c, &d), imm);
                if (d.reg != 7) set_rm16(c, &d, r);
            } else {
                uint8_t imm = fetch8(c, &d);
                uint8_t r = alu8(c, d.reg, get_rm8(c, &d), imm);
                if (d.reg != 7) set_rm8(c, &d, r);
            }
            break;
        case 0x84: decode_modrm(c, &d); alu_and8(c, get_rm8(c, &d), *reg8(c, d.reg)); break;
        case 0x85: decode_modrm(c, &d); alu_and16(c, get_rm16(c, &d), *reg16(c, d.reg)); break;
        case 0x86:
            decode_modrm(c, &d);
            {
                uint8_t t = get_rm8(c, &d);
                set_rm8(c, &d, *reg8(c, d.reg));
                *reg8(c, d.reg) = t;
            }
            break;
        case 0x87:
            decode_modrm(c, &d);
            {
                uint16_t t = get_rm16(c, &d);
                set_rm16(c, &d, *reg16(c, d.reg));
                *reg16(c, d.reg) = t;
            }
            break;
        case 0x88: decode_modrm(c, &d); set_rm8(c, &d, *reg8(c, d.reg)); break;
        case 0x89: decode_modrm(c, &d); set_rm16(c, &d, *reg16(c, d.reg)); break;
        case 0x8A: decode_modrm(c, &d); *reg8(c, d.reg) = get_rm8(c, &d); break;
        case 0x8B: decode_modrm(c, &d); *reg16(c, d.reg) = get_rm16(c, &d); break;
        case 0x8C: decode_modrm(c, &d); set_rm16(c, &d, *sreg(c, d.reg)); break;
        case 0x8D: decode_modrm(c, &d); *reg16(c, d.reg) = d.ea_off; break;
        case 0x8E:
            decode_modrm(c, &d);
            if ((d.reg & 3) == 1) unsupported(c, start, op);
            *sreg(c, d.reg) = get_rm16(c, &d);
            break;
        case 0x8F: {
            uint16_t value = pop16(c);
            decode_modrm(c, &d);
            set_rm16(c, &d, value);
            break;
        }
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t t = c->ax.x;
            c->ax.x = *reg16(c, op);
            *reg16(c, op) = t;
            break;
        }
        case 0x98: op_cbw(c); break;
        case 0x99: op_cwd(c); break;
        case 0x9A: {
            uint16_t offset = fetch16(c, &d);
            uint16_t segment = fetch16(c, &d);
            uint16_t caller_cs = c->cs;
            push16(c, c->cs);
            push16(c, d.ip);
            rt_call_far(c, segment, offset);
            c->cs = caller_cs;
            if (LEFT_FRAME()) return;
            c->cs = c->ret_cs;
            d.ip = c->ret_ip;
            break;
        }
        case 0x9B: break;
        case 0x9C: op_pushf(c); break;
        case 0x9D: op_popf(c); rt_poll_interrupts(c); break;
        case 0x9E: op_sahf(c); break;
        case 0x9F: op_lahf(c); break;
        case 0xA0: c->ax.b.l = rd8(c, data_segment(c, &d), fetch16(c, &d)); break;
        case 0xA1: c->ax.x = rd16(c, data_segment(c, &d), fetch16(c, &d)); break;
        case 0xA2: wr8(c, data_segment(c, &d), fetch16(c, &d), c->ax.b.l); break;
        case 0xA3: wr16(c, data_segment(c, &d), fetch16(c, &d), c->ax.x); break;
        case 0xA8: alu_and8(c, c->ax.b.l, fetch8(c, &d)); break;
        case 0xA9: alu_and16(c, c->ax.x, fetch16(c, &d)); break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            *reg8(c, op) = fetch8(c, &d);
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            *reg16(c, op) = fetch16(c, &d);
            break;
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            decode_modrm(c, &d);
            {
                uint8_t count = (op == 0xC0 || op == 0xC1) ? fetch8(c, &d) : (op <= 0xD1 ? 1 : c->cx.b.l);
                if (op & 1) {
                    set_rm16(c, &d, shift16(c, d.reg, get_rm16(c, &d), count));
                } else {
                    set_rm8(c, &d, shift8(c, d.reg, get_rm8(c, &d), count));
                }
            }
            break;
        case 0xC2: case 0xC3: {
            uint16_t extra = op == 0xC2 ? fetch16(c, &d) : 0;
            c->ret_ip = pop16(c);
            c->sp += extra;
            if (LEFT_FRAME()) return;
            d.ip = c->ret_ip;
            break;
        }
        case 0xC4: case 0xC5:
            decode_modrm(c, &d);
            *reg16(c, d.reg) = rd16(c, d.ea_seg, d.ea_off);
            if (op == 0xC4) {
                c->es = rd16(c, d.ea_seg, (uint16_t)(d.ea_off + 2));
            } else {
                c->ds = rd16(c, d.ea_seg, (uint16_t)(d.ea_off + 2));
            }
            break;
        case 0xC6: decode_modrm(c, &d); set_rm8(c, &d, fetch8(c, &d)); break;
        case 0xC7: decode_modrm(c, &d); set_rm16(c, &d, fetch16(c, &d)); break;
        case 0xC8: {
            uint16_t size = fetch16(c, &d);
            uint8_t level = fetch8(c, &d);
            op_enter(c, size, level);
            break;
        }
        case 0xC9: c->sp = c->bp; c->bp = pop16(c); break;
        case 0xCA: case 0xCB: {
            uint16_t extra = op == 0xCA ? fetch16(c, &d) : 0;
            c->ret_ip = pop16(c);
            c->ret_cs = pop16(c);
            c->sp += extra;
            if (LEFT_FRAME()) return;
            c->cs = c->ret_cs;
            d.ip = c->ret_ip;
            break;
        }
        case 0xCC: case 0xCD: case 0xCE: {
            uint8_t number = op == 0xCC ? 3 : op == 0xCD ? fetch8(c, &d) : 4;
            if (op == 0xCE && !FLAG_OF(c)) break;
            rt_int(c, number);
            break;
        }
        case 0xCF:
            c->ret_ip = pop16(c);
            c->ret_cs = pop16(c);
            c->flags = pop16(c);
            if (LEFT_FRAME()) return;
            c->cs = c->ret_cs;
            d.ip = c->ret_ip;
            break;
        case 0xD4: {
            uint8_t base = fetch8(c, &d);
            if (!base) rt_fatal("interpreter: AAM 0 at %04x:%04x", c->cs, start);
            c->ax.b.h = c->ax.b.l / base;
            c->ax.b.l = c->ax.b.l % base;
            set_szp8_flags(c, c->ax.b.l);
            break;
        }
        case 0xD5: {
            uint8_t base = fetch8(c, &d);
            c->ax.b.l = (uint8_t)(c->ax.b.h * base + c->ax.b.l);
            c->ax.b.h = 0;
            set_szp8_flags(c, c->ax.b.l);
            break;
        }
        case 0xD7: {
            uint16_t segment = data_segment(c, &d);
            c->ax.b.l = rd8(c, segment, (uint16_t)(c->bx.x + c->ax.b.l));
            break;
        }
        case 0xE0: case 0xE1: case 0xE2: {
            int8_t displacement = (int8_t)fetch8(c, &d);
            c->cx.x--;
            int taken = c->cx.x != 0;
            if (op == 0xE0) taken = taken && !FLAG_ZF(c);
            if (op == 0xE1) taken = taken && FLAG_ZF(c);
            if (taken) {
                d.ip = (uint16_t)(d.ip + displacement);
                if (displacement < 0) rt_poll_loop(c, d.ip);
            }
            break;
        }
        case 0xE3: {
            int8_t displacement = (int8_t)fetch8(c, &d);
            if (c->cx.x == 0) d.ip = (uint16_t)(d.ip + displacement);
            break;
        }
        case 0xE4: c->ax.b.l = rt_in8(c, fetch8(c, &d)); break;
        case 0xE5: c->ax.x = rt_in16(c, fetch8(c, &d)); break;
        case 0xE6: rt_out8(c, fetch8(c, &d), c->ax.b.l); break;
        case 0xE7: rt_out16(c, fetch8(c, &d), c->ax.x); break;
        case 0xEC: c->ax.b.l = rt_in8(c, c->dx.x); break;
        case 0xED: c->ax.x = rt_in16(c, c->dx.x); break;
        case 0xEE: rt_out8(c, c->dx.x, c->ax.b.l); break;
        case 0xEF: rt_out16(c, c->dx.x, c->ax.x); break;
        case 0xE8: {
            int16_t displacement = (int16_t)fetch16(c, &d);
            uint16_t target = (uint16_t)(d.ip + displacement);
            push16(c, d.ip);
            interpreted_call_near(c, target);
            if (LEFT_FRAME()) return;
            d.ip = c->ret_ip;
            break;
        }
        case 0xE9: case 0xEB: {
            int16_t displacement = op == 0xE9 ? (int16_t)fetch16(c, &d) : (int8_t)fetch8(c, &d);
            d.ip = (uint16_t)(d.ip + displacement);
            if (displacement < 0) rt_poll_loop(c, d.ip);
            break;
        }
        case 0xEA: {
            uint16_t offset = fetch16(c, &d);
            uint16_t segment = fetch16(c, &d);
            rt_jump_far(c, segment, offset);
            return;
        }
        case 0xF4: rt_halt(c); break;
        case 0xF5: c->flags ^= F_CF; break;
        case 0xF6: case 0xF7:
            decode_modrm(c, &d);
            if (op == 0xF6) {
                uint8_t v = get_rm8(c, &d);
                switch (d.reg) {
                case 0: case 1: alu_and8(c, v, fetch8(c, &d)); break;
                case 2: set_rm8(c, &d, (uint8_t)~v); break;
                case 3: set_rm8(c, &d, alu_neg8(c, v)); break;
                case 4: op_mul8(c, v); break;
                case 5: op_imul8(c, v); break;
                case 6: op_div8(c, v); break;
                default: op_idiv8(c, v); break;
                }
            } else {
                uint16_t v = get_rm16(c, &d);
                switch (d.reg) {
                case 0: case 1: alu_and16(c, v, fetch16(c, &d)); break;
                case 2: set_rm16(c, &d, (uint16_t)~v); break;
                case 3: set_rm16(c, &d, alu_neg16(c, v)); break;
                case 4: op_mul16(c, v); break;
                case 5: op_imul16(c, v); break;
                case 6: op_div16(c, v); break;
                default: op_idiv16(c, v); break;
                }
            }
            break;
        case 0xF8: c->flags &= ~F_CF; break;
        case 0xF9: c->flags |= F_CF; break;
        case 0xFA: c->flags &= ~F_IF; break;
        case 0xFB: c->flags |= F_IF; rt_poll_interrupts(c); break;
        case 0xFC: c->flags &= ~F_DF; break;
        case 0xFD: c->flags |= F_DF; break;
        case 0xFE:
            decode_modrm(c, &d);
            if (d.reg == 0) {
                set_rm8(c, &d, alu_inc8(c, get_rm8(c, &d)));
            } else if (d.reg == 1) {
                set_rm8(c, &d, alu_dec8(c, get_rm8(c, &d)));
            } else {
                unsupported(c, start, op);
            }
            break;
        case 0xFF:
            decode_modrm(c, &d);
            switch (d.reg) {
            case 0: set_rm16(c, &d, alu_inc16(c, get_rm16(c, &d))); break;
            case 1: set_rm16(c, &d, alu_dec16(c, get_rm16(c, &d))); break;
            case 2: {
                uint16_t target = get_rm16(c, &d);
                push16(c, d.ip);
                interpreted_call_near(c, target);
                if (LEFT_FRAME()) return;
                d.ip = c->ret_ip;
                break;
            }
            case 3: {
                uint16_t offset = rd16(c, d.ea_seg, d.ea_off);
                uint16_t segment = rd16(c, d.ea_seg, (uint16_t)(d.ea_off + 2));
                uint16_t caller_cs = c->cs;
                push16(c, c->cs);
                push16(c, d.ip);
                rt_call_far(c, segment, offset);
                c->cs = caller_cs;
                if (LEFT_FRAME()) return;
                c->cs = c->ret_cs;
                d.ip = c->ret_ip;
                break;
            }
            case 4: {
                uint16_t target = get_rm16(c, &d);
                RtFunc func = c->module ? rt_find_function(c->module, (uint16_t)(c->cs - c->load_seg), target) : 0;
                if (func) {
                    func(c);
                    return;
                }
                d.ip = target;
                break;
            }
            case 5: {
                uint16_t offset = rd16(c, d.ea_seg, d.ea_off);
                uint16_t segment = rd16(c, d.ea_seg, (uint16_t)(d.ea_off + 2));
                rt_jump_far(c, segment, offset);
                return;
            }
            case 6: push16(c, get_rm16(c, &d)); break;
            default: unsupported(c, start, op);
            }
            break;
        default:
            unsupported(c, start, op);
        }
        next = d.ip;
        ip = next;
        (void)entry_cs;
        (void)entry_module;
        (void)entry_load;
    }
}
