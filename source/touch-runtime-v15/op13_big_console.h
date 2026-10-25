/* Two-times bitmap console. Scroll only within the supplied rectangle. */
#ifndef OP13_BIG_CONSOLE_H
#define OP13_BIG_CONSOLE_H
#include <stdint.h>
#include <stddef.h>
#include "op13_console_font.h"

enum { OP13_CELL_WIDTH = 18, OP13_CELL_HEIGHT = 36, OP13_CONSOLE_MARGIN = 16 };
struct op13_big_console {
    volatile uint32_t *pixels;
    uint32_t width, stride, top, rows, columns, row, column;
};

static void op13_console_clear(struct op13_big_console *c, uint32_t top, uint32_t bottom)
{
    for (uint32_t y = top; y < bottom; y++)
        for (uint32_t x = 0; x < c->width; x++)
            c->pixels[(size_t)y * c->stride + x] = 0;
}

static int op13_console_init(struct op13_big_console *c, void *pixels, uint64_t bytes,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t top, uint32_t bottom)
{
    *c = (struct op13_big_console){0};
    if (!pixels || ((uintptr_t)pixels & 3) || width < OP13_CELL_WIDTH + 2 * OP13_CONSOLE_MARGIN ||
        width > 8192 || height > 8192 || stride % 4 || stride / 4 < width ||
        (uint64_t)stride * height > bytes || top >= bottom || bottom > height ||
        bottom - top < OP13_CELL_HEIGHT) return 0;
    c->pixels = pixels; c->width = width; c->stride = stride / 4; c->top = top;
    c->rows = (bottom - top) / OP13_CELL_HEIGHT;
    c->columns = (width - 2 * OP13_CONSOLE_MARGIN) / OP13_CELL_WIDTH;
    op13_console_clear(c, top, top + c->rows * OP13_CELL_HEIGHT);
    return 1;
}

static void op13_console_newline(struct op13_big_console *c)
{
    /* Erase the rest of the completed text row, including any splash pixels.
     * Once logs cross the logo it is replaced in place, never scrolled as debris. */
    uint32_t left = OP13_CONSOLE_MARGIN + c->column * OP13_CELL_WIDTH;
    uint32_t line = c->top + c->row * OP13_CELL_HEIGHT;
    for (uint32_t y = line; y < line + OP13_CELL_HEIGHT; y++)
        for (uint32_t x = left; x < c->width; x++)
            c->pixels[(size_t)y * c->stride + x] = 0;
    c->column = 0;
    if (++c->row < c->rows) return;
    uint32_t bottom = c->top + c->rows * OP13_CELL_HEIGHT;
    for (uint32_t y = c->top; y < bottom - OP13_CELL_HEIGHT; y++)
        for (uint32_t x = 0; x < c->width; x++)
            c->pixels[(size_t)y * c->stride + x] =
                c->pixels[(size_t)(y + OP13_CELL_HEIGHT) * c->stride + x];
    op13_console_clear(c, bottom - OP13_CELL_HEIGHT, bottom);
    c->row = c->rows - 1;
}

static void op13_console_char(struct op13_big_console *c, unsigned char ch)
{
    if (!c->pixels) return;
    if (ch == '\r') { c->column = 0; return; }
    if (ch == '\n') { op13_console_newline(c); return; }
    if (ch == '\t') {
        unsigned count = 4 - c->column % 4;
        while (count--) op13_console_char(c, ' ');
        return;
    }
    if (ch < 32 || ch > 126) ch = '?';
    if (c->column >= c->columns) op13_console_newline(c);
    uint32_t left = OP13_CONSOLE_MARGIN + c->column * OP13_CELL_WIDTH;
    uint32_t top = c->top + c->row * OP13_CELL_HEIGHT;
    for (uint32_t y = 0; y < OP13_CELL_HEIGHT; y++) {
        uint8_t bits = y < 32 ? op13_console_font[(unsigned)ch * 16 + y / 2] : 0;
        for (uint32_t x = 0; x < OP13_CELL_WIDTH; x++)
            c->pixels[(size_t)(top + y) * c->stride + left + x] =
                x < 16 && (bits & (1U << (x / 2))) ? 0xffffff : 0;
    }
    c->column++;
}
#endif
