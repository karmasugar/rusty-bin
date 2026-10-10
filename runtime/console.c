/* PC-98 DOS console: characters written through DOS go to text VRAM (80x25 cells, codes at A0000h, attributes
 * at A2000h) as on the real machine, and are copied to the host terminal. Handles CR, LF, BS, ASCII and half-width
 * kana, Shift-JIS kanji (two cells), scrolling, and the escape sequences the game uses: ESC* (clear screen),
 * ESC[2J, ESC[K, ESC[y;xH, ESC[..m (colors), ESC=yx; other ESC[ sequences (such as ESC[>1h) are ignored. */
#include "rt.h"

#include <stdio.h>

#define ROWS 25
#define COLUMNS 80
#define WHITE 0xE1

static int row;
static int column;
static uint8_t attribute = WHITE;
static uint8_t lead_byte;
static int escape_state;
static char escape[32];
static int escape_length;

static void print_host(uint8_t ch)
{
    if (ch == '\r') return;
    if (ch == '\n' || (ch >= 0x20 && ch < 0x7F)) {
        putchar(ch);
    } else {
        printf("\\x%02x", ch);
    }
}

static void put_cell(Cpu *c, int r, int col, uint16_t code, uint8_t attr)
{
    uint32_t cell = (uint32_t)(r * COLUMNS + col) * 2;
    c->mem[0xA0000 + cell] = (uint8_t)code;
    c->mem[0xA0001 + cell] = (uint8_t)(code >> 8);
    c->mem[0xA2000 + cell] = attr;
}

static void clear_cells(Cpu *c, int from, int to)
{
    for (int i = from; i < to; i++) put_cell(c, i / COLUMNS, i % COLUMNS, 0x0020, WHITE);
}

static void new_line(Cpu *c)
{
    column = 0;
    if (++row < ROWS) return;
    row = ROWS - 1;
    for (uint32_t i = 0; i < (ROWS - 1) * COLUMNS * 2; i++) {
        c->mem[0xA0000 + i] = c->mem[0xA0000 + COLUMNS * 2 + i];
        c->mem[0xA2000 + i] = c->mem[0xA2000 + COLUMNS * 2 + i];
    }
    clear_cells(c, (ROWS - 1) * COLUMNS, ROWS * COLUMNS);
}

static void advance(Cpu *c)
{
    if (++column >= COLUMNS) new_line(c);
}

static int parameter(int index, int fallback)
{
    int value = 0, current = 0, seen = 0;
    for (int i = 1; i < escape_length - 1; i++) {
        char ch = escape[i];
        if (ch == ';') {
            if (current == index) return seen ? value : fallback;
            current++;
            value = 0;
            seen = 0;
        } else if (ch >= '0' && ch <= '9') {
            value = value * 10 + (ch - '0');
            seen = 1;
        }
    }
    return current == index && seen ? value : fallback;
}

static void set_color(int code)
{
    static const uint8_t colors[8] = {0x00, 0x40, 0x80, 0xC0, 0x20, 0x60, 0xA0, 0xE0};
    if (code == 0) attribute = WHITE;
    if (code == 7) attribute |= 0x04;
    if (code >= 30 && code <= 37) attribute = (uint8_t)((attribute & 0x1F) | colors[code - 30]);
}

static void run_escape(Cpu *c)
{
    char final = escape[escape_length - 1];
    if (escape[1] == '>' || escape[1] == '?') return;
    switch (final) {
    case 'J':
        clear_cells(c, 0, ROWS * COLUMNS);
        row = column = 0;
        break;
    case 'K':
        clear_cells(c, row * COLUMNS + column, (row + 1) * COLUMNS);
        break;
    case 'H':
    case 'f':
        row = parameter(0, 1) - 1;
        column = parameter(1, 1) - 1;
        if (row < 0 || row >= ROWS) row = 0;
        if (column < 0 || column >= COLUMNS) column = 0;
        break;
    case 'm':
        for (int i = 0; i < 8; i++) {
            int code = parameter(i, -1);
            if (code < 0) break;
            set_color(code);
        }
        break;
    default:
        break;
    }
}

static int escape_byte(Cpu *c, uint8_t ch)
{
    if (escape_state == 1) {
        if (ch == '*') {
            clear_cells(c, 0, ROWS * COLUMNS);
            row = column = 0;
            escape_state = 0;
        } else if (ch == '[') {
            escape_state = 2;
            escape[0] = '[';
            escape_length = 1;
        } else if (ch == '=') {
            escape_state = 3;
        } else {
            escape_state = 0;
        }
        return 1;
    }
    if (escape_state == 2) {
        if (escape_length < (int)sizeof(escape)) escape[escape_length++] = (char)ch;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            run_escape(c);
            escape_state = 0;
        }
        return 1;
    }
    if (escape_state == 3) {
        row = (ch - 0x20) % ROWS;
        escape_state = 4;
        return 1;
    }
    if (escape_state == 4) {
        column = (ch - 0x20) % COLUMNS;
        escape_state = 0;
        return 1;
    }
    return 0;
}

static void put_kanji(Cpu *c, uint8_t first, uint8_t second)
{
    int jis_first = ((first - (first <= 0x9F ? 0x70 : 0xB0)) << 1) - (second < 0x9F ? 1 : 0);
    int jis_second = second < 0x9F ? second - 0x1F - (second >= 0x80 ? 1 : 0) : second - 0x7E;
    uint16_t cell = (uint16_t)(((jis_first - 0x20) & 0x7F) | (jis_second << 8));
    if (column >= COLUMNS - 1) new_line(c);
    put_cell(c, row, column, cell, attribute);
    put_cell(c, row, column + 1, (uint16_t)(cell | 0x80), attribute);
    column += 2;
    if (column >= COLUMNS) new_line(c);
}

void console_write(Cpu *c, uint8_t ch)
{
    print_host(ch);
    if (escape_state && escape_byte(c, ch)) return;
    if (lead_byte) {
        put_kanji(c, lead_byte, ch);
        lead_byte = 0;
        return;
    }
    if ((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC)) {
        lead_byte = ch;
        return;
    }
    switch (ch) {
    case 0x1B:
        escape_state = 1;
        return;
    case '\r':
        column = 0;
        return;
    case '\n':
        new_line(c);
        column = 0;
        return;
    case 0x08:
        if (column > 0) column--;
        return;
    case 0x07:
        return;
    default:
        put_cell(c, row, column, ch, attribute);
        advance(c);
    }
}

void console_snapshot(StateStream *s)
{
    STATE(s, row);
    STATE(s, column);
    STATE(s, attribute);
    STATE(s, lead_byte);
    STATE(s, escape_state);
    STATE(s, escape);
    STATE(s, escape_length);
}
