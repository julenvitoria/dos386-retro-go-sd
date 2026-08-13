/*
 * CGA video.
 *
 * VRAM is a dedicated 32 KB block, not pool pages. It is mapped once at guest
 * 0xB8000 and never moves, so the guest writes the framebuffer as plain memory
 * and nothing needs an MMIO fault path: a CGA program pokes B800:0 and reads it
 * back, and both work because it really is RAM. The conversion to the LCD
 * happens once per frame instead of once per store, which is the whole reason
 * this is affordable.
 *
 * Budget (AXI, 1024 KB total): 154 KB LUT8 framebuffer, 32 KB VRAM here, and
 * the remaining ~838 KB is the guest RAM pool. A DOS game in conventional
 * memory wants 640 KB of that.
 */
#include <stdint.h>
#include <string.h>

#include "cutils.h"
#include "x86_cpu.h"
#include "font8x8.h"

/*
 * Text mode 3 is 80x25 in 8x8 cells: 640x200, which does not fit a 320x240
 * panel. We render 8x8 cells into a 320x200 area, i.e. 40 columns and 25 rows,
 * and scroll the visible window horizontally with the cursor. Halving each
 * glyph instead would make 80 columns fit but leaves text unreadable at this
 * size, and DOS output is mostly the left-hand columns anyway.
 */
#define TEXT_COLS       80
#define TEXT_ROWS       25
#define TEXT_VIS_COLS   40
#define CELL_W          8
#define CELL_H          8

/* Mode 4 planes: even scanlines at +0x0000, odd at +0x2000, 80 bytes a row. */
#define CGA_PLANE_ODD   0x2000u
#define CGA_ROW_BYTES   80u
#define CGA_WIDTH       320u
#define CGA_HEIGHT      200u

void cga_mode4_blit(uint8_t *dst, const uint8_t *even, const uint8_t *odd,
                    const uint32_t *tab, uint32_t dst_advance);

static uint8_t *cga_vram;
static void text_render(uint8_t *dst, uint32_t stride);
/* 2bpp -> four palette bytes. In DTCM: the blitter hits it once per source
   byte, and DTCM's second port keeps that off the AXI path the source is on. */
static uint32_t cga_expand[256] __attribute__((section(".dtcm_bss")));

/* The 16 CGA colours in hardware index order, as ARGB8888.
 *
 * The LTDC CLUT is 32-bit, NOT RGB565 -- lcd_set_clut() takes const uint32_t*
 * and caps the count at 32. Handing it a uint16_t array of RGB565 gives a
 * black screen, because every pair of entries is reinterpreted as one ARGB
 * word. */
static const uint32_t cga_argb[16] = {
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
};

static uint32_t *cga_clut;      /* the LUT8 CLUT the LCD scans out through */
/* Set whenever the four active colours change. The frame loop re-pushes the
   CLUT on it: the firmware copies our table into its own, so editing ours
   after the fact would never reach the panel. */
uint32_t cga_clut_dirty;

/*
 * Recompute the four active colours from the mode/colour-select registers.
 *
 * Mode 4 takes colour 0 from the background field of 0x3D9 -- it is a real
 * register, not a constant, and games change it -- while colours 1..3 come
 * from the palette pair selected by bit 5, brightened by bit 4. The
 * black-and-white bit in 0x3D8 selects a third set on real hardware.
 */
void x86_video_refresh_palette(void)
{
    if (!cga_clut)
        return;

    unsigned sel = x86_io.cga_color_select;
    unsigned background = sel & 0x0F;
    unsigned bright = (sel & 0x10) ? 8u : 0u;

    unsigned c1, c2, c3;
    if (x86_io.cga_mode & 0x04) {          /* composite/BW: cyan, red, white */
        c1 = 3; c2 = 4; c3 = 7;
    } else if (sel & 0x20) {               /* palette 1: cyan, magenta, white */
        c1 = 3; c2 = 5; c3 = 7;
    } else {                               /* palette 0: green, red, brown */
        c1 = 2; c2 = 4; c3 = 6;
    }

    if (x86_video_is_text()) {
        /* Attributes index all 16 colours directly, so the CLUT is the whole
           CGA palette rather than the four graphics entries. */
        for (unsigned i = 0; i < 16; i++)
            cga_clut[i] = cga_argb[i];
        cga_clut_dirty = 1;
        return;
    }

    cga_clut[0] = cga_argb[background];
    cga_clut[1] = cga_argb[(c1 | bright) & 15];
    cga_clut[2] = cga_argb[(c2 | bright) & 15];
    /* Brown is the odd one out: index 6 brightens to 14 (yellow); white (7)
       brightens to 15, which the mask already gives. */
    cga_clut[3] = cga_argb[(c3 | bright) & 15];
    cga_clut_dirty = 1;
}

void x86_video_init(void *vram, void *lut8_palette)
{
    cga_vram = (uint8_t *)vram;
    cga_clut = (uint32_t *)lut8_palette;
    memset(cga_vram, 0, X86_VRAM_BYTES);

    /* CGA packs the leftmost pixel in the HIGH bits of each byte. The table is
       built so a little-endian word store lands them left to right. */
    for (unsigned b = 0; b < 256; b++) {
        cga_expand[b] = ((b >> 6) & 3)
                      | (((b >> 4) & 3) << 8)
                      | (((b >> 2) & 3) << 16)
                      | (((b >> 0) & 3) << 24);
    }

    /* Power-on state: mode 4 graphics with the bright cyan/magenta/white
       palette, which is what the BIOS leaves behind. The guest overrides it
       through 0x3D8/0x3D9 and the CLUT follows. */
    x86_io.cga_mode = 0x0A;
    x86_io.cga_color_select = 0x30;
    x86_video_refresh_palette();
}

/* Map VRAM into the guest at 0xB8000. Fixed, not pooled: it is a device
   aperture that happens to be readable memory, and it must not move. */
int x86_video_map(void)
{
    return x86_map_fixed(0xB8000u, cga_vram, X86_VRAM_BYTES);
}

/* Where the beam is, as a fraction of the frame. The core loop advances this
   so a guest waiting on vertical retrace waits roughly a frame, not forever
   and not zero time. */
void x86_video_set_beam(uint32_t frac)
{
    x86_io.cga_beam = (uint16_t)frac;
}

uint8_t *x86_video_vram(void)
{
    return cga_vram;
}

/*
 * One frame. `dst` is the LUT8 framebuffer, `stride` its pitch in bytes.
 * 320x200 lands inside a 320x240 panel, so the picture is centred with 20
 * blank rows above and below rather than stretched -- CGA pixels are not
 * square, but scaling costs a multiply per pixel and looks worse.
 */
void x86_video_render(uint8_t *dst, uint32_t stride)
{
    /* Clear the letterbox. 320x200 into a 240-row panel leaves 20 rows top
       and bottom; leaving them alone means they keep whatever the firmware or
       the other buffer had, reinterpreted through the CGA palette -- which is
       what turns an otherwise black screen into colour flashes. */
    unsigned band = (240u - CGA_HEIGHT) / 2u;
    memset(dst, 0, band * stride);
    memset(dst + (band + CGA_HEIGHT) * stride, 0, band * stride);

    uint8_t *top = dst + band * stride;
    if (x86_video_is_text())
        text_render(top, stride);
    else
        cga_mode4_blit(top, cga_vram, cga_vram + CGA_PLANE_ODD,
                       cga_expand, stride - CGA_WIDTH);
}

/* ---------------------------------------------------------------------------
 * Mode and cursor state.
 */
static uint8_t  video_mode = 0x04;
static uint8_t  cur_row, cur_col;
static uint16_t text_scroll_col;    /* leftmost visible column */

/*
 * The panel is 320 px wide and the font is 8 px, so 40 of the 80 text columns
 * fit. The window normally follows the cursor, which is right for a program
 * that prints and then waits -- but wrong for one that rewrites a wide line and
 * returns the cursor to column 0, because the part it just wrote scrolls back
 * out of view. TOPBENCH's score sits at column 46.
 *
 * Set this to a column to pin the window there instead; -1 restores following
 * the cursor. Written by scripts/screenshot.py --pan. A user-facing pan (on a
 * button) is the real fix and is not this.
 */
volatile int32_t x86_video_pan = -1;

void x86_video_set_mode(uint8_t mode)
{
    video_mode = mode;
    cur_row = cur_col = 0;
    text_scroll_col = 0;
    memset(cga_vram, (mode == 0x03 || mode == 0x02) ? 0 : 0, X86_VRAM_BYTES);
    if (mode == 0x03 || mode == 0x02) {
        /* Text mode: blank cells are space on black, not zero bytes -- a zero
           attribute renders black-on-black and hides everything written
           before the guest sets its own colours. */
        uint16_t *cell = (uint16_t *)cga_vram;
        for (unsigned i = 0; i < TEXT_COLS * TEXT_ROWS; i++)
            cell[i] = 0x0720;
    }
    x86_video_refresh_palette();
}

uint8_t x86_video_mode(void) { return video_mode; }

void x86_video_set_cursor(uint8_t col, uint8_t row)
{
    cur_col = col; cur_row = row;
    if (cur_col < text_scroll_col)
        text_scroll_col = cur_col;
    else if (cur_col >= text_scroll_col + TEXT_VIS_COLS)
        text_scroll_col = (uint16_t)(cur_col - TEXT_VIS_COLS + 1);
}

uint16_t x86_video_get_cursor(void)
{
    return (uint16_t)((cur_row << 8) | cur_col);
}

static uint16_t *text_cell(unsigned row, unsigned col)
{
    return (uint16_t *)cga_vram + row * TEXT_COLS + col;
}

/* The cell under the cursor, character in the low byte and attribute in the
   high byte -- the layout INT 10h AH=08 returns in AX. */
uint16_t x86_video_read_cell(void)
{
    if (!cga_vram)
        return 0x0720;
    return *text_cell(cur_row, cur_col);
}

void x86_video_scroll(uint8_t lines, uint8_t attr)
{
    if (lines == 0 || lines >= TEXT_ROWS) {
        uint16_t *c = (uint16_t *)cga_vram;
        for (unsigned i = 0; i < TEXT_COLS * TEXT_ROWS; i++)
            c[i] = (uint16_t)(0x20 | (attr << 8));
        return;
    }
    memmove(cga_vram, cga_vram + lines * TEXT_COLS * 2,
            (TEXT_ROWS - lines) * TEXT_COLS * 2);
    uint16_t *c = text_cell(TEXT_ROWS - lines, 0);
    for (unsigned i = 0; i < lines * TEXT_COLS; i++)
        c[i] = (uint16_t)(0x20 | (attr << 8));
}

void x86_video_putc_attr(uint8_t ch, uint8_t attr, uint16_t count)
{
    if (count == 0) count = 1;
    for (unsigned i = 0; i < count && cur_col + i < TEXT_COLS; i++)
        *text_cell(cur_row, cur_col + i) = (uint16_t)(ch | (attr << 8));
}

void x86_video_teletype(uint8_t ch)
{
    switch (ch) {
    case 0x0D: cur_col = 0; break;
    case 0x0A: cur_row++; break;
    case 0x08: if (cur_col) cur_col--; break;
    default:
        *text_cell(cur_row, cur_col) =
            (uint16_t)(ch | ((*text_cell(cur_row, cur_col) >> 8) << 8));
        if (++cur_col >= TEXT_COLS) { cur_col = 0; cur_row++; }
        break;
    }
    if (cur_row >= TEXT_ROWS) {
        x86_video_scroll(1, 0x07);
        cur_row = TEXT_ROWS - 1;
    }
    x86_video_set_cursor(cur_col, cur_row);
}

void x86_video_putpixel(uint16_t x, uint16_t y, uint8_t colour)
{
    if (x >= CGA_WIDTH || y >= CGA_HEIGHT)
        return;
    uint32_t off = ((y & 1) ? CGA_PLANE_ODD : 0) + (y >> 1) * CGA_ROW_BYTES
                 + (x >> 2);
    unsigned shift = 6 - 2 * (x & 3);
    cga_vram[off] = (uint8_t)((cga_vram[off] & ~(3u << shift))
                              | ((colour & 3u) << shift));
}

/* INT 10h AH=0Bh: BH=0 sets the background/border, BH=1 the palette. Both land
   in the same colour-select register the ports write. */
void x86_video_set_palette_bios(uint8_t which, uint8_t value)
{
    if (which == 0)
        x86_io.cga_color_select = (uint8_t)((x86_io.cga_color_select & 0xF0)
                                            | (value & 0x0F));
    else
        x86_io.cga_color_select = (uint8_t)((x86_io.cga_color_select & ~0x20)
                                            | ((value & 1) ? 0x20 : 0));
    x86_video_refresh_palette();
}

/* ---------------------------------------------------------------------------
 * Text rendering. One glyph row is eight 1-bit pixels; the attribute byte
 * gives foreground in the low nibble and background in the high, both indexing
 * the 16-colour CGA palette. Text mode therefore needs all 16 CLUT entries
 * where mode 4 needs four.
 */
static void text_render(uint8_t *dst, uint32_t stride)
{
    for (unsigned row = 0; row < TEXT_ROWS; row++) {
        unsigned left = text_scroll_col;
        if (x86_video_pan >= 0)
            left = (unsigned)x86_video_pan;
        if (left > TEXT_COLS - TEXT_VIS_COLS)
            left = TEXT_COLS - TEXT_VIS_COLS;
        for (unsigned vc = 0; vc < TEXT_VIS_COLS; vc++) {
            unsigned col = left + vc;
            uint16_t cell = (col < TEXT_COLS) ? *text_cell(row, col) : 0x0720;
            const unsigned char *glyph = font8x8_basic[cell & 0x7F];
            uint8_t fg = (uint8_t)((cell >> 8) & 0x0F);
            uint8_t bg = (uint8_t)((cell >> 12) & 0x07);

            for (unsigned gy = 0; gy < CELL_H; gy++) {
                uint8_t bits = glyph[gy];
                uint8_t *o = dst + (row * CELL_H + gy) * stride + vc * CELL_W;
                /* font8x8 stores the leftmost pixel in bit 0. */
                for (unsigned gx = 0; gx < CELL_W; gx++)
                    o[gx] = (bits & (1u << gx)) ? fg : bg;
            }
        }
    }
}

int x86_video_is_text(void)
{
    return video_mode == 0x02 || video_mode == 0x03 || video_mode == 0x00
        || video_mode == 0x01;
}
