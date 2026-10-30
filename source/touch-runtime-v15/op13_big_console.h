/* Two-times bitmap console. Scroll only within the supplied rectangle. */
#ifndef OP13_BIG_CONSOLE_H
#define OP13_BIG_CONSOLE_H
#include <stdint.h>
#include <stddef.h>
#include "op13_console_font.h"

enum { OP13_CELL_WIDTH = 18, OP13_CELL_HEIGHT = 36, OP13_CONSOLE_MARGIN = 16, OP13_HISTORY_LINES = 512, OP13_HISTORY_COLUMNS = 454 };
struct op13_big_console {
    volatile uint32_t *pixels;
    uint32_t width, stride, top, rows, columns, row, column;
    uint64_t history_line;
    uint32_t history_scroll;
    unsigned char history[OP13_HISTORY_LINES][OP13_HISTORY_COLUMNS];
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
        ((uint64_t)(height-1)*stride+(uint64_t)width*4) > bytes || top >= bottom || bottom > height ||
        bottom - top < OP13_CELL_HEIGHT) return 0;
    c->pixels = pixels; c->width = width; c->stride = stride / 4; c->top = top;
    c->rows = (bottom - top) / OP13_CELL_HEIGHT;
    c->columns = (width - 2 * OP13_CONSOLE_MARGIN) / OP13_CELL_WIDTH;
    op13_console_clear(c, top, top + c->rows * OP13_CELL_HEIGHT);
    return 1;
}

/* Text history stays separate from the displayed pixels. Offset zero follows
 * live output; nonzero offsets hold the same lines while new output arrives. */
static unsigned op13_console_max_scroll(const struct op13_big_console *c)
{
    uint64_t available=c->history_line+1;
    if(available>OP13_HISTORY_LINES)available=OP13_HISTORY_LINES;
    return available>c->rows?(unsigned)(available-c->rows):0;
}
static void op13_console_cell(struct op13_big_console *c,unsigned row,unsigned column,unsigned char ch)
{
    if(ch<32||ch>126)ch=' ';
    uint32_t left=OP13_CONSOLE_MARGIN+column*OP13_CELL_WIDTH;
    uint32_t top=c->top+row*OP13_CELL_HEIGHT;
    for(uint32_t y=0;y<OP13_CELL_HEIGHT;y++){
        uint8_t bits=y<32?op13_console_font[(unsigned)ch*16+y/2]:0;
        for(uint32_t x=0;x<OP13_CELL_WIDTH;x++)
            c->pixels[(size_t)(top+y)*c->stride+left+x]=
                x<16&&(bits&(1U<<(x/2)))?0xffffff:0;
    }
}
static void op13_console_redraw(struct op13_big_console *c)
{
    op13_console_clear(c,c->top,c->top+c->rows*OP13_CELL_HEIGHT);
    uint64_t first=c->history_line+1>c->rows?c->history_line+1-c->rows:0;
    first-=c->history_scroll;
    for(unsigned row=0;row<c->rows&&first+row<=c->history_line;row++)
        for(unsigned col=0;col<c->columns;col++){
            unsigned char ch=c->history[(first+row)%OP13_HISTORY_LINES][col];
            if(ch)op13_console_cell(c,row,col,ch);
        }
}
static int op13_console_scroll(struct op13_big_console *c,int lines)
{
    if(!c->pixels||!lines)return 0;
    int64_t target=(int64_t)c->history_scroll+lines;
    unsigned max=op13_console_max_scroll(c);
    if(target<0)target=0;if(target>max)target=max;
    if((unsigned)target==c->history_scroll)return 0;
    c->history_scroll=(unsigned)target;op13_console_redraw(c);return 1;
}

static void op13_console_newline(struct op13_big_console *c)
{
    /* Erase the rest of the completed text row, including any splash pixels.
     * Once logs cross the logo it is replaced in place, never scrolled as debris. */
    for(unsigned col=c->column;col<c->columns;col++)c->history[c->history_line%OP13_HISTORY_LINES][col]=0;
    c->history_line++;
    for(unsigned col=0;col<c->columns;col++)c->history[c->history_line%OP13_HISTORY_LINES][col]=0;
    if(c->history_scroll){
        unsigned old=c->history_scroll;
        c->history_scroll++;unsigned max=op13_console_max_scroll(c);
        if(c->history_scroll>max)c->history_scroll=max;
        c->column=0;if(c->row+1<c->rows)c->row++;
        if(c->history_scroll==old)op13_console_redraw(c); /* oldest line evicted */
        return;
    }
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
    c->history[c->history_line%OP13_HISTORY_LINES][c->column]=ch;
    if(!c->history_scroll)op13_console_cell(c,c->row,c->column,ch);
    c->column++;
}
#endif
